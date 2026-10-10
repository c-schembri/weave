#include <weave/postgres.hpp>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <array>
#include <cerrno>
#include <pwd.h>

namespace pg = weave::pg;

enum class Lookup {
  success,
  missing,
  alias,
  reverse_alias,
  large,
  exhausted,
  error,
  reverse_missing,
  reverse_error
};
static Lookup lookup = Lookup::success;
static unsigned calls = 0;
static std::size_t largest_buffer = 0;

extern "C" int __wrap_getpwnam_r(const char *, passwd *record, char *, std::size_t size, passwd **found)
{
  ++calls;
  largest_buffer = std::max(largest_buffer, size);
  *found = nullptr;
  if (lookup == Lookup::missing)
    return 0;
  if (lookup == Lookup::error)
    return EIO;
  if (lookup == Lookup::exhausted || (lookup == Lookup::large && size < 16384))
    return ERANGE;

  *record = {};
  record->pw_uid = 12345;
  record->pw_name = const_cast<char *>(lookup == Lookup::alias ? "other" : "weave-peer");
  *found = record;
  return 0;
}

extern "C" int __wrap_getpwuid_r(uid_t user, passwd *record, char *, std::size_t, passwd **found)
{
  ++calls;
  *found = nullptr;
  if (lookup == Lookup::reverse_missing)
    return 0;
  if (lookup == Lookup::reverse_error)
    return EIO;

  *record = {};
  record->pw_uid = user;
  record->pw_name = const_cast<char *>(lookup == Lookup::reverse_alias ? "other" : "weave-peer");
  *found = record;
  return 0;
}

static weave::Result<pg::Options> load(Lookup scenario)
{
  lookup = scenario;
  calls = 0;
  largest_buffer = 0;
  pg::ConfigSources sources{.environment = false, .user_files = false};
  return pg::Options::load("user=app sslmode=disable requirepeer=weave-peer", sources);
}

TEST_CASE("PostgreSQL peer-name configuration snapshots canonical UID with bounded NSS growth")
{
  const std::array accepted{Lookup::success, Lookup::large};
  for (auto scenario : accepted) {
    auto options = load(scenario);
    REQUIRE(options);
    CHECK(options->required_peer_user == 12345);
    CHECK(calls >= 2);
    if (scenario == Lookup::large)
      CHECK(largest_buffer == 16384);
  }
}

TEST_CASE("PostgreSQL peer-name configuration rejects missing names, aliases and NSS errors")
{
  const std::array rejected{
    std::pair{Lookup::missing, std::errc::no_such_file_or_directory},
    std::pair{Lookup::alias, std::errc::permission_denied},
    std::pair{Lookup::reverse_alias, std::errc::permission_denied},
    std::pair{Lookup::error, std::errc::io_error},
    std::pair{Lookup::reverse_missing, std::errc::no_such_file_or_directory},
    std::pair{Lookup::reverse_error, std::errc::io_error}};
  for (const auto &[scenario, error] : rejected) {
    auto options = load(scenario);
    REQUIRE_FALSE(options);
    CHECK(options.error() == error);
    CHECK(calls > 0);
    CHECK(calls <= 2);
  }

  auto exhausted = load(Lookup::exhausted);
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error().value() == ERANGE);
  CHECK(largest_buffer == 65536);
  CHECK(calls == 5);
}

TEST_CASE("PostgreSQL empty peer policy and pure parsing never perform NSS lookup")
{
  lookup = Lookup::error;
  calls = 0;
  pg::ConfigSources sources{.environment = false, .user_files = false};
  auto empty = pg::Options::load("user=app requirepeer=''", sources);
  REQUIRE(empty);
  CHECK_FALSE(empty->required_peer_user);
  auto parsed = pg::Options::parse("user=app requirepeer=weave-peer");
  REQUIRE_FALSE(parsed);
  CHECK(parsed.error() == std::errc::operation_not_supported);
  CHECK(calls == 0);
}
