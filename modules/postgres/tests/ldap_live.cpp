#include <weave/postgres.hpp>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <string>

namespace pg = weave::pg;
static unsigned checks = 0;

static void check(bool condition, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "LDAP service-lookup check failed at line %u\n", location.line());
    std::abort();
  }
}

int main(int argc, char **argv)
{
  check(argc == 6);
  std::filesystem::path directory{argv[1]};
  auto file = directory / "ldap.conf";
  std::string endpoint{argv[2]};
  std::string unavailable{argv[3]};
  std::string stalled{argv[5]};
  auto mode = std::string_view{argv[4]};
  bool enabled = mode != "disabled";
  pg::ConfigSources sources{
    .environment = false,
    .user_files = false,
    .service_file = file.string(),
    .ldap = enabled,
    .ldap_timeout = std::chrono::milliseconds{500}};
  auto load = [&](std::string text, std::string_view options = "service=sample") {
    {
      std::ofstream output(file);
      output << "[sample]\n" << text;
      check(static_cast<bool>(output));
    }
    return pg::Options::load(options, sources);
  };
  const std::string normal = endpoint + "/cn=normal,dc=weave,dc=test?description?base?(objectClass=*)";
  if (!enabled || mode == "compiled-off") {
    auto rejected = load(normal + "\nuser=fallback\n");
    check(!rejected && rejected.error() == std::errc::operation_not_supported);
    std::puts("LDAP-disabled control passed (no native lookup)");
    return 0;
  }
  auto positive = load(normal + "\nuser=ignored\n", "service=sample application_name=explicit");
  if (!positive)
    std::fprintf(
      stderr,
      "LDAP positive failure: %s (%d)\n",
      positive.error().message().c_str(),
      positive.error().value());
  check(static_cast<bool>(positive));
  check(positive->user == "directory_user");
  check(positive->host == "127.0.0.1");
  check(positive->database == "directory_database");
  check(positive->password == "directory secret");
  check(positive->application_name == "explicit");
  check(positive->plaintext);
  auto precedence = load("user=before\n" + normal + "\n", "service=sample application_name=explicit");
  check(precedence && precedence->user == "before");

  const std::array scoped{std::string{"one"}, std::string{"sub"}};
  for (const auto &scope : scoped) {
    auto selected = load(endpoint + "/dc=weave,dc=test?description?" + scope + "?(cn=normal)\n");
    check(selected && selected->user == "directory_user");
  }
  auto encoded = load(endpoint + "/cn%3Dnormal%2Cdc%3Dweave%2Cdc%3Dtest?description?BASE?%28objectClass%3D%2A%29\n");
  check(encoded && encoded->user == "directory_user");
  auto fallback = load(
    unavailable + "/cn=normal,dc=weave,dc=test?description?base?(objectClass=*)\nuser=fallback\nsslmode=disable\n");
  check(fallback && fallback->user == "fallback");
  auto second = load(unavailable + "/cn=normal,dc=weave,dc=test?description?base?(objectClass=*)\n" + normal + "\n");
  check(second && second->user == "directory_user");

  const std::array bad_queries{
    "/cn=absent,dc=weave,dc=test?description?base?(objectClass=*)",
    "/dc=weave,dc=test?description?one?(objectClass=device)",
    "/cn=missing,dc=weave,dc=test?description?base?(objectClass=*)",
    "/cn=bad,dc=weave,dc=test?description?base?(objectClass=*)",
    "/cn=nested,dc=weave,dc=test?description?base?(objectClass=*)",
    "/cn=nul,dc=weave,dc=test?userPassword?base?(objectClass=*)",
    "/cn=large,dc=weave,dc=test?description?base?(objectClass=*)",
    "/cn=aggregate,dc=weave,dc=test?userPassword?base?(objectClass=*)",
    "/cn=separators,dc=weave,dc=test?description?base?(objectClass=*)",
    "/cn=many,dc=weave,dc=test?description?base?(objectClass=*)",
    "/cn=referral,dc=weave,dc=test?description?base?(objectClass=*)",
    "/cn=normal,dc=weave,dc=test?description?base?(broken",
    "/cn=normal,dc=weave,dc=test?description,cn?base?(objectClass=*)",
    "/cn=normal,dc=weave,dc=test?description?invalid?(objectClass=*)",
    "/cn=normal,dc=weave,dc=test?description?base?(objectClass=*)?bindname=x",
    "/cn=normal,dc=weave,dc=test?description?base?%00"};
  for (auto query : bad_queries) {
    auto rejected = load(endpoint + query + "\nuser=fallback\nsslmode=disable\n");
    if (rejected)
      std::fprintf(stderr, "LDAP unexpectedly accepted query %s\n", query);
    check(!rejected);
  }
  auto empty = load(endpoint + "/cn=empty,dc=weave,dc=test?description?base?(objectClass=*)\n");
  check(empty && empty->password.empty() && empty->user == "empty_user");
  auto nested = load(normal + "\nservice=nested\n");
  check(static_cast<bool>(nested));

  auto multi = load(endpoint + "/cn=multi,dc=weave,dc=test?description?base?(objectClass=*)\n");
  check(multi && multi->user == "multi_user" && multi->database == "multi_database" && multi->plaintext);
  auto zero = load(
    "user=zero_user\nsslmode=disable\n" + stalled +
    "/cn=zero,dc=weave,dc=test?description?base?(objectClass=*)\nuser=ignored\n");
  check(zero && zero->user == "zero_user");
  auto utf8 = load(endpoint + "/cn=utf8,dc=weave,dc=test?description?base?(objectClass=*)\n");
  check(utf8 && utf8->application_name == "caf\xc3\xa9");
  auto unicode = load(endpoint + "/cn=caf%C3%A9,dc=weave,dc=test?description?base?(objectClass=*)\n");
  check(unicode && unicode->user == "unicode_user");
  auto limit = load(
    endpoint + "/cn=valid-limit,dc=weave,dc=test?description?base?(objectClass=*)\n",
    "service=sample user=app");
  check(limit && limit->application_name.size() == 65000 && limit->plaintext);
  auto allowed = load(
    "user=app\nsslmode=disable\n" + endpoint + "/cn=allowed,dc=weave,dc=test?description?base?(objectClass=*)\n");
  check(allowed && !allowed->application_name.empty());

  const std::array invalid_urls{
    "ldap://256.256.256.256:389/cn=normal?description?base?(objectClass=*)",
    "ldap://127.1:389/cn=normal?description?base?(objectClass=*)",
    "ldap://user@127.0.0.1:389/cn=normal?description?base?(objectClass=*)",
    "ldap://127.0.0.1:0/cn=normal?description?base?(objectClass=*)",
    "ldap://127.0.0.1:65536/cn=normal?description?base?(objectClass=*)",
    "ldap://127.0.0.1:389/?description?base?(objectClass=*)",
    "ldap://127.0.0.1:389/cn=normal?description?base?(objectClass=*)#fragment",
    "ldap://127.0.0.1:389/cn=normal?description?base",
    "ldap://127.0.0.1:389/cn=normal?description%2Ccn?base?(objectClass=*)",
    "ldap://127.0.0.1:389/cn%GGnormal?description?base?(objectClass=*)",
    "ldaps://127.0.0.1:636/cn=normal?description?base?(objectClass=*)"};
  for (auto query : invalid_urls) {
    auto rejected = load(std::string{query} + "\nuser=fallback\nsslmode=disable\n");
    check(!rejected);
  }

  std::string exhausted;
  for (unsigned index = 0; index < 17; ++index)
    exhausted += unavailable + "/cn=normal,dc=weave,dc=test?description?base?(objectClass=*)\n";
  auto bounded = load(exhausted + "user=fallback\nsslmode=disable\n");
  check(!bounded && bounded.error() == std::errc::value_too_large);
  auto start = std::chrono::steady_clock::now();
  auto deadline = load(
    stalled +
    "/cn=normal,dc=weave,dc=test?description?base?(objectClass=*)\n"
    "user=fallback\nsslmode=disable\n");
  auto elapsed = std::chrono::steady_clock::now() - start;
  if (!deadline)
    std::printf(
      "Search stall rejected: %s (%d), elapsed %lld ms\n",
      deadline.error().category().name(),
      deadline.error().value(),
      static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
  check(!deadline);
  check(elapsed >= std::chrono::milliseconds{100} && elapsed < std::chrono::seconds{5});
  sources.ldap = false;
  auto disabled = load(normal + "\nuser=fallback\n");
  check(!disabled && disabled.error() == std::errc::operation_not_supported);
  sources.ldap = true;
  sources.ldap_timeout = std::chrono::milliseconds{0};
  auto invalid = load(normal + "\n");
  check(!invalid && invalid.error() == std::errc::invalid_argument);
  std::error_code cleanup;
  check(std::filesystem::remove(file, cleanup) && !cleanup);
  std::printf("LDAP service-lookup checks passed: %u checks\n", checks);
}
