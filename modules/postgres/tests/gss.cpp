#include "gss.hpp"
#include <doctest/doctest.h>
#include <array>
#include <type_traits>

namespace pg = weave::pg;
namespace native = weave::pg::detail;

TEST_CASE("GSS targets preserve host/service boundaries and bound provider input")
{
  const std::array hosts{"localhost", "database.example", "127.0.0.1", "::1", "2001:db8::1"};
  const std::array services{"postgres", "POSTGRES", "postgres_18", "database-service", "service.name"};
  for (auto host : hosts) {
    for (auto service : services)
      CHECK(native::valid_gss_target(host, service));
  }

  const std::array invalid_hosts{
    "",
    "host/instance",
    "host\\instance",
    "host@realm",
    "host name",
    "host\tname",
    "host\nname",
    "host\x7f"};
  const std::array invalid_services{"", "service/instance", "service@realm", "service name", "service:port"};
  for (auto host : invalid_hosts)
    CHECK_FALSE(native::valid_gss_target(host, "postgres"));
  for (auto service : invalid_services)
    CHECK_FALSE(native::valid_gss_target("localhost", service));
  CHECK_FALSE(native::valid_gss_target(std::string_view{"host\0suffix", 11}, "postgres"));
  CHECK_FALSE(native::valid_gss_target("localhost", std::string_view{"service\0suffix", 14}));

  CHECK(native::valid_gss_target(std::string(65535, 'a'), std::string(256, 'b')));
  CHECK_FALSE(native::valid_gss_target(std::string(65536, 'a'), "postgres"));
  CHECK_FALSE(native::valid_gss_target("localhost", std::string(257, 'b')));
}

TEST_CASE("GSS rejects unstarted continuation and invalid starts without credential discovery")
{
  static_assert(!std::is_copy_constructible_v<native::Gss>);
  static_assert(!std::is_move_constructible_v<native::Gss>);
  native::Gss provider;
  CHECK_FALSE(provider.complete());
  CHECK(provider.diagnostic().empty());
  auto pending = provider.next({});
  REQUIRE_FALSE(pending);
  CHECK(pending.error() == pg::Error::authentication);

  const std::array methods{
    pg::Authentication::none,
    pg::Authentication::password,
    pg::Authentication::md5,
    pg::Authentication::scram_sha256,
    pg::Authentication::oauth};
  for (auto method : methods) {
    auto result = provider.start("localhost", method, {});
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::invalid_argument);
  }

  native::GssOptions options;
  options.service = "postgres/attacker";
  auto service = provider.start("localhost", pg::Authentication::gss, options);
  REQUIRE_FALSE(service);
  CHECK(service.error() == std::errc::invalid_argument);
  auto host = provider.start("localhost@attacker", pg::Authentication::gss, {});
  REQUIRE_FALSE(host);
  CHECK(host.error() == std::errc::invalid_argument);
  CHECK_FALSE(provider.complete());
}

TEST_CASE("GSS availability matches its private platform build selection")
{
#if defined(_WIN32) || defined(WEAVE_POSTGRES_TEST_GSSAPI)
  CHECK(native::Gss::available());
#else
  CHECK_FALSE(native::Gss::available());
  native::Gss provider;
  auto result = provider.start("localhost", pg::Authentication::gss, {});
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_not_supported);
  CHECK_FALSE(provider.complete());
#endif
}

TEST_CASE("GSS provider context validates bounded explicit setup before creating workers")
{
  static_assert(!std::is_default_constructible_v<pg::GssContext>);
  static_assert(std::is_copy_constructible_v<pg::GssContext>);
  const std::array invalid{
    pg::GssContextOptions{.workers = 0},
    pg::GssContextOptions{.workers = 65},
    pg::GssContextOptions{.capacity = 1},
    pg::GssContextOptions{.capacity = 65537},
    pg::GssContextOptions{.credential_cache = std::string(65537, 'x')},
    pg::GssContextOptions{.credential_cache = std::string{"cache\0other", 11}}};
  for (const auto &options : invalid) {
    auto rejected = pg::GssContext::create(options);
    CHECK((!rejected && rejected.error() == std::errc::invalid_argument));
  }
#if defined(_WIN32)
  auto cache = pg::GssContext::create({.credential_cache = "FILE:unsupported"});
  CHECK((!cache && cache.error() == std::errc::operation_not_supported));
#elif !defined(WEAVE_POSTGRES_TEST_GSSAPI)
  auto unavailable = pg::GssContext::create();
  CHECK((!unavailable && unavailable.error() == std::errc::operation_not_supported));
#endif
}

TEST_CASE("GSS record protection cannot run without a completed protected context")
{
  native::Gss provider;
  CHECK(provider.plaintext_limit() == 0);
  const std::array bytes{std::byte{0x41}};
  auto wrapped = provider.wrap(bytes);
  auto unwrapped = provider.unwrap(bytes);
  REQUIRE_FALSE(wrapped);
  REQUIRE_FALSE(unwrapped);
#if defined(_WIN32) || defined(WEAVE_POSTGRES_TEST_GSSAPI)
  CHECK(wrapped.error() == pg::Error::authentication);
  CHECK(unwrapped.error() == pg::Error::authentication);
#else
  CHECK(wrapped.error() == std::errc::operation_not_supported);
  CHECK(unwrapped.error() == std::errc::operation_not_supported);
#endif
  CHECK_FALSE(provider.complete());
  CHECK(provider.diagnostic().empty());
}

TEST_CASE("GSS encrypted connection setup requires an explicit provider before submitting I/O")
{
  const std::array modes{pg::GssEncryption::prefer, pg::GssEncryption::require};
  auto context = weave::Context::create();
  REQUIRE(context);
  for (auto mode : modes) {
    pg::Options options{.user = "test", .plaintext = true};
    options.gss_encryption = mode;
    auto result = context->run(pg::connect(std::move(options)));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_not_supported);
  }
  CHECK(context->metrics().submitted == 0);
}
