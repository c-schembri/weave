#include <weave/port.hpp>
#include <doctest/doctest.h>
#include <ostream>
#include <type_traits>

static_assert(std::is_same_v<decltype(weave::parse_port("8080")), std::expected<std::uint16_t, std::error_code>>);
static_assert(noexcept(weave::parse_port("8080")));

TEST_CASE("Port parsing accepts the full decimal range including zero")
{
  struct PortCase {
    std::string_view text;
    std::uint16_t value;
  };

  for (auto [text, value] : {PortCase{"0", 0}, {"1", 1}, {"8080", 8080}, {"65535", 65535}, {"00080", 80}}) {
    CAPTURE(text);
    auto port = weave::parse_port(text);
    REQUIRE(port);
    CHECK(*port == value);
  }
}

TEST_CASE("Port parsing rejects malformed and out-of-range input without partial acceptance")
{
  for (std::string_view text :
    {"",
      "-1",
      "-0",
      "+80",
      " 80",
      "80 ",
      "80\n",
      "\t80",
      "http",
      "80x",
      "8.0",
      "0x50",
      "65536",
      "4294967296",
      "99999999999999999999999999999999999999999999"}) {
    CAPTURE(text);
    auto port = weave::parse_port(text);
    REQUIRE_FALSE(port);
    CHECK(port.error() == std::errc::invalid_argument);
  }

  auto empty = weave::parse_port(std::string_view{});
  REQUIRE_FALSE(empty);
  CHECK(empty.error() == std::errc::invalid_argument);
}

TEST_CASE("Port parsing respects string view bounds rather than null termination")
{
  constexpr char text[] = {'8', '0', '8', '0', 'x'};
  CHECK(weave::parse_port(std::string_view{text, 4}) == 8080);
  CHECK_FALSE(weave::parse_port(std::string_view{text, 5}));

  auto embedded_null = weave::parse_port(std::string_view{"80\0x", 4});
  REQUIRE_FALSE(embedded_null);
  CHECK(embedded_null.error() == std::errc::invalid_argument);
}
