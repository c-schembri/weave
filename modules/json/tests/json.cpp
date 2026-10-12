#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/json.hpp>
#include <array>
#include <optional>

struct Shipment {
  int id = 0;
  double weight = 0;
  bool delivered = false;
  std::string recipient;
};

struct Renamed {
  int id;
};

template <>
struct glz::meta<Renamed> {
  static constexpr auto value = glz::object("shipment_id", &Renamed::id);
};

TEST_CASE("JSON reads ordinary aggregates and honors native Glaze metadata")
{
  auto shipment = weave::json::read<Shipment>(R"({"id":42,"weight":3.5,"delivered":true,"recipient":"O'Reilly"})");
  REQUIRE(shipment);
  if (!shipment)
    return;
  CHECK(shipment->id == 42);
  CHECK(shipment->weight == 3.5);
  CHECK(shipment->delivered);
  CHECK(shipment->recipient == "O'Reilly");
  auto named = weave::json::read<Renamed>(R"({"shipment_id":7})");
  CHECK(named.has_value());
  if (named)
    CHECK(named->id == 7);
}

TEST_CASE("JSON reusable and fixed buffers produce matching documents")
{
  Shipment shipment{42, 3.5, true, "recipient"};
  auto owned = weave::json::write(shipment);
  REQUIRE(owned);
  if (!owned)
    return;

  std::string reused;
  reused.reserve(512);
  auto *storage = reused.data();
  CHECK(weave::json::write(shipment, reused));
  CHECK(reused == *owned);
  CHECK(reused.data() == storage);
  std::array<char, 512> fixed;
  auto count = weave::json::write(shipment, fixed);
  REQUIRE(count);
  if (!count)
    return;
  CHECK(std::string_view(fixed.data(), *count) == *owned);

  std::array<char, 16> guarded;
  guarded.fill('x');
  auto too_small = weave::json::write(shipment, std::span{guarded}.subspan(4, 2));
  CHECK_FALSE(too_small);
  CHECK(guarded[3] == 'x');
  CHECK(guarded[6] == 'x');
  CHECK_FALSE(weave::json::write(shipment, std::span<char>{}));
}

TEST_CASE("JSON diagnostics preserve offsets without capturing input and reset on success")
{
  constexpr std::string_view input = R"({"id":42,"recipient":"sensitive-text",})";
  weave::json::Failure failure;
  auto invalid = weave::json::read<Shipment>(input, &failure);
  REQUIRE_FALSE(invalid);
  if (invalid)
    return;
  CHECK(failure.error == invalid.error());
  CHECK(failure.offset > 0);
  CHECK(failure.offset <= input.size());
  CHECK(failure.message.find("sensitive-text") == std::string::npos);
  CHECK(std::string_view{failure.error.category().name()} == "weave.json");
  CHECK(weave::json::read<Shipment>("{}", &failure));
  CHECK_FALSE(failure.error);
  CHECK(failure.offset == 0);
  CHECK(failure.message.empty());
}

TEST_CASE("JSON validates complete documents and rejects unknown typed fields")
{
  const std::array malformed{
    "",
    "{",
    "{} trailing",
    "{} {}",
    "{/*comment*/}",
    "{\"id\":01}",
    "{\"recipient\":\"\xff\"}",
    "{\"recipient\":\"\\ud800\"}",
    "{\"unknown\":42}"};
  for (auto input : malformed)
    CHECK_FALSE(weave::json::read<Shipment>(input));
  CHECK(weave::json::read<Shipment>(" \n {} \t\r\n"));
  constexpr std::string_view bounded{"{}\0", 3};
  CHECK_FALSE(weave::json::read<Shipment>(bounded));
}

TEST_CASE("JSON in-place decoding retains missing fields and owning decoding initializes them")
{
  Shipment shipment{42, 3.5, true, "recipient"};
  CHECK(weave::json::read(shipment, R"({"weight":7.5})"));
  CHECK(shipment.id == 42);
  CHECK(shipment.weight == 7.5);
  auto empty = weave::json::read<Shipment>("{}");
  CHECK(empty.has_value());
  if (empty)
    CHECK(empty->id == 0);
}
