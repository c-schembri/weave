#include "windows/address.hpp"
#include <doctest/doctest.h>
#include <array>

TEST_CASE("IP addresses parse, format and roundtrip native endpoints without Winsock startup")
{
  constexpr std::array addresses{
    "0.0.0.0",
    "127.0.0.1",
    "255.255.255.255",
    "::",
    "::1",
    "2001:db8::1234",
    "fe80::1%12",
    "::ffff:127.0.0.1"};
  constexpr std::array ports{weave::u16{0}, weave::u16{65535}};

  for (const char *text : addresses) {
    CAPTURE(text);
    auto address = weave::IpAddress::parse(text);
    REQUIRE(address);
    CHECK(weave::IpAddress::parse(address->to_string()) == address);
    for (weave::u16 port : ports) {
      weave::Endpoint endpoint{*address, port};
      auto native = weave::detail::socket_address(endpoint);
      CHECK(weave::detail::socket_endpoint(native.data(), native.size) == endpoint);
      CHECK_FALSE(weave::detail::socket_endpoint(native.data(), native.size - 1));
    }
  }
  CHECK(weave::IpAddress::any_v4().to_string() == "0.0.0.0");
  CHECK(weave::IpAddress::any_v6().to_string() == "::");
  CHECK(weave::IpAddress::loopback_v4().to_string() == "127.0.0.1");
  CHECK(weave::IpAddress::loopback_v6().to_string() == "::1");
  CHECK(weave::Endpoint{weave::IpAddress::loopback_v6(), 8080}.to_string() == "[::1]:8080");
  CHECK(weave::Endpoint{weave::IpAddress::loopback_v4(), 8080}.to_string() == "127.0.0.1:8080");
  CHECK_FALSE(weave::detail::socket_endpoint(nullptr, 0));
}

TEST_CASE("Invalid or ambiguous numeric address syntax is rejected")
{
  constexpr std::array invalid_addresses{
    "",
    "localhost",
    "127.1",
    "256.0.0.1",
    "127.0.0.1:80",
    "[::1]",
    "::1%",
    "::1%eth0",
    "::1%4294967296",
    "::1%1%2",
    "127.0.0.1%2",
    ":::1"};

  for (const char *text : invalid_addresses) {
    CAPTURE(text);
    auto address = weave::IpAddress::parse(text);
    REQUIRE_FALSE(address);
    CHECK(address.error() == std::errc::invalid_argument);
  }
  CHECK_FALSE(weave::IpAddress::parse(std::string_view("127.0.0.1\0junk", 14)));
}
