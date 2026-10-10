#include <weave/postgres.hpp>
#include "ldap.hpp"
#include "options.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <array>

namespace pg = weave::pg;
namespace detail = weave::pg::detail;

TEST_CASE("LDAP URLs have one validated authority and explicit search scope")
{
  auto implicit = detail::ldap_url("LDAP:///cn=normal?description?base?(objectClass=*)");
  REQUIRE(implicit);
  CHECK(implicit->host == "localhost");
  CHECK(implicit->port == 389);

  auto ipv6 = detail::ldap_url("ldap://[::1]:1234/cn=normal?description?sub?(objectClass=*)");
  REQUIRE(ipv6);
  CHECK(ipv6->host == "::1");
  CHECK(ipv6->port == 1234);
  CHECK(ipv6->ipv6);
  CHECK(ipv6->scope == detail::LdapScope::sub);

  auto encoded = detail::ldap_url("ldap://ldap.example:389/cn%3Dcaf%C3%A9?description?ONE?%28cn%3D%2A%29");
  REQUIRE(encoded);
  CHECK(encoded->base == "cn=caf\xc3\xa9");
  CHECK(encoded->filter == "(cn=*)");
  CHECK(encoded->scope == detail::LdapScope::one);

  auto literal = detail::ldap_url("ldap://ldap.example./cn=a+b?description?BASE?(cn=a+b)");
  REQUIRE(literal);
  CHECK(literal->base == "cn=a+b");
  CHECK(literal->filter == "(cn=a+b)");
}

TEST_CASE("LDAP URL extensions and ambiguous or malformed destinations fail")
{
  const std::array invalid_urls{
    "ldaps://localhost/cn=x?description?base?(cn=*)",
    "ldap://localhost/cn=x?description?base?(cn=*)?ignored-extension",
    "ldap://user@localhost/cn=x?description?base?(cn=*)",
    "ldap://[v1.test]/cn=x?description?base?(cn=*)",
    "ldap://[::1%251]/cn=x?description?base?(cn=*)",
    "ldap://host..example/cn=x?description?base?(cn=*)",
    "ldap://-host.example/cn=x?description?base?(cn=*)",
    "ldap://host_.example/cn=x?description?base?(cn=*)",
    "ldap://localhost/cn=x??base?(cn=*)",
    "ldap://localhost/cn=x?description?base?",
    "ldap://localhost/cn=x?description?subtree?(cn=*)",
    "ldap://localhost/cn=x?description,cn?base?(cn=*)",
    "ldap://localhost/cn=x?description?base?(cn=*)#fragment",
    "ldap://localhost/cn=x?description?base?%00"};
  for (auto input : invalid_urls) {
    INFO(input);
    CHECK_FALSE(detail::ldap_url(input));
  }
}

TEST_CASE("LDAP options preserve first values and empty unquoted values")
{
  detail::OptionFields fields;
  REQUIRE(
    detail::parse_service_options(
      fields,
      "password=\nuser=first\nuser=second\napplication_name='a\\'b'\noptions=a\\b\n"));
  REQUIRE(fields.get("user"));
  CHECK(*fields.get("user") == "first");
  REQUIRE(fields.get("password"));
  CHECK(fields.get("password")->empty());
  REQUIRE(fields.get("application_name"));
  CHECK(*fields.get("application_name") == "a'b");
  REQUIRE(fields.get("options"));
  CHECK(*fields.get("options") == "a\\b");

  const std::array
    invalid_options{"service=recursive", "unknown=ignored", "user\n=bad", "user='unterminated", "user='quoted'bad"};
  for (auto input : invalid_options) {
    INFO(input);
    detail::OptionFields rejected;
    CHECK_FALSE(detail::parse_service_options(rejected, input));
  }
}

TEST_CASE("LDAP service grammar does not change ordinary connection-string grammar")
{
  auto unchanged = pg::Options::parse("user=first user=second options=a\\b password=\nuser=third");
  REQUIRE(unchanged);
  CHECK(unchanged->user == "second");
  CHECK(unchanged->password == "user=third");
  CHECK(unchanged->server_options == "ab");
}

TEST_CASE("LDAP setup requires a bounded positive native timeout")
{
  pg::ConfigSources sources{.environment = false, .user_files = false, .ldap = true};
  sources.ldap_timeout = std::chrono::milliseconds{0};
  auto zero = pg::Options::load("user=app", sources);
  REQUIRE_FALSE(zero);
  CHECK(zero.error() == std::errc::invalid_argument);
  sources.ldap_timeout = std::chrono::minutes{2};
  auto excessive = pg::Options::load("user=app", sources);
  REQUIRE_FALSE(excessive);
  CHECK(excessive.error() == std::errc::invalid_argument);
}

#ifndef WEAVE_POSTGRES_TEST_LDAP
TEST_CASE("Compiled-out LDAP cannot perform a native lookup")
{
  auto query = detail::ldap_url("ldap://127.0.0.1:1/cn=x?description?base?(cn=*)");
  REQUIRE(query);
  auto result = detail::ldap_query(*query, std::chrono::milliseconds{500});
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_not_supported);
}
#endif
