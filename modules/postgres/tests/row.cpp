#include <weave/postgres.hpp>
#include "wire.hpp"
#include "result_access.hpp"
#include <doctest/doctest.h>
#include <array>
#include <cmath>

namespace pg = weave::pg;
namespace storage = weave::pg::detail;

static pg::Value text(std::string_view bytes)
{
  return {storage::ResultText{bytes}};
}

static pg::Value binary(weave::u64 bits, std::size_t size, weave::u32 type)
{
  std::array<char, 8> bytes{};
  for (std::size_t index = 0; index < size; ++index)
    bytes[index] = static_cast<char>((bits >> ((size - index - 1) * 8)) & 0xff);
  return {storage::ResultText{std::string_view{bytes.data(), size}}, pg::Format::binary, type};
}

template <class T>
static void rejects(const weave::Result<T> &result, pg::DecodeError error)
{
  CHECK_FALSE(result);
  if (!result)
    CHECK(result.error() == pg::make_error_code(error));
}

static pg::ResultSet sample()
{
  pg::ResultSet result;
  result.columns.push_back({.name = "id", .type = 20});
  result.columns.push_back({.name = "weight", .type = 701});
  result.columns.push_back({.name = "delivered", .type = 16});
  result.columns.push_back({.name = "recipient", .type = 25});
  result.columns.push_back({.name = "notes", .type = 25});
  result.rows.push_back(pg::Row{text("42"), text("1.25"), text("t"), text("Ada"), pg::Value{}});
  storage::ResultAccess::bind_rows(result);
  return result;
}

TEST_CASE("PostgreSQL primitive text conversions preserve types ranges and NULL")
{
  CHECK(text("-128").as<weave::i8>() == -128);
  CHECK(text("255").as<weave::u8>() == 255);
  CHECK(text("-32768").as<weave::i16>() == -32768);
  CHECK(text("4294967295").as<weave::u32>() == 4294967295ULL);
  CHECK(text("-9223372036854775808").as<weave::i64>() == std::numeric_limits<weave::i64>::min());
  CHECK(text("18446744073709551615").as<weave::u64>() == std::numeric_limits<weave::u64>::max());
  rejects(text("128").as<weave::i8>(), pg::DecodeError::out_of_range);
  rejects(text("256").as<weave::u8>(), pg::DecodeError::out_of_range);
  rejects(text("9223372036854775808").as<weave::i64>(), pg::DecodeError::out_of_range);
  rejects(text("18446744073709551616").as<weave::u64>(), pg::DecodeError::out_of_range);
  rejects(text("-1").as<weave::u64>(), pg::DecodeError::invalid_value);

  const std::array malformed{"", " 42", "42 ", "+42", "42x", "4.2"};
  for (const auto *input : malformed)
    rejects(text(input).as<int>(), pg::DecodeError::invalid_value);

  CHECK(text("1.25").as<float>() == 1.25F);
  CHECK(text("-1.25e2").as<double>() == -125.0);
  rejects(text("1e1000").as<double>(), pg::DecodeError::out_of_range);
  rejects(text("1e-1000").as<float>(), pg::DecodeError::out_of_range);
  rejects(text("1.25kg").as<double>(), pg::DecodeError::invalid_value);
  auto nan = text("NaN").as<double>();
  auto infinity = text("Infinity").as<float>();
  auto negative_infinity = text("-Infinity").as<double>();
  auto zero = text("-0").as<double>();
  CHECK((nan && std::isnan(*nan)));
  CHECK((infinity && std::isinf(*infinity) && *infinity > 0));
  CHECK((negative_infinity && std::isinf(*negative_infinity) && *negative_infinity < 0));
  CHECK((zero && std::signbit(*zero)));

  CHECK(text("t").as<bool>() == true);
  CHECK(text("f").as<bool>() == false);
  rejects(text("true").as<bool>(), pg::DecodeError::invalid_value);
  rejects(text("1").as<bool>(), pg::DecodeError::invalid_value);
  pg::Value null;
  rejects(null.as<int>(), pg::DecodeError::null_value);
  rejects(null.as<std::string_view>(), pg::DecodeError::null_value);
  auto optional = null.as<std::optional<int>>();
  CHECK((optional && !*optional));
  auto present = text("42").as<std::optional<int>>();
  CHECK((present && *present == std::optional<int>{42}));
  rejects(text("bad").as<std::optional<int>>(), pg::DecodeError::invalid_value);
  auto empty = text("").as<std::optional<std::string>>();
  CHECK((empty && empty->has_value() && (**empty).empty()));

  const std::string bytes{"a\0b", 3};
  auto value = text(bytes);
  auto view = value.as<std::string_view>();
  CHECK((view && *view == std::string_view{bytes}));
  CHECK((view && view->data() == value.data->data()));
  auto owned = value.as<std::string>();
  value.data->assign("changed");
  CHECK(owned == bytes);
}

TEST_CASE("PostgreSQL binary primitive conversion uses source OID and width")
{
  CHECK(binary(0x8000, 2, 21).as<weave::i64>() == -32768);
  CHECK(binary(42, 4, 23).as<weave::i64>() == 42);
  CHECK(binary(0x80000000, 4, 23).as<weave::i64>() == std::numeric_limits<weave::i32>::min());
  CHECK(binary(0x8000000000000000ULL, 8, 20).as<weave::i64>() == std::numeric_limits<weave::i64>::min());
  CHECK(binary(0xffffffff, 4, 26).as<weave::u64>() == 4294967295ULL);
  CHECK(binary(42, 4, 23).as<double>() == 42.0);
  rejects(binary(256, 4, 23).as<weave::u8>(), pg::DecodeError::out_of_range);
  rejects(binary(0xffff, 2, 21).as<weave::u64>(), pg::DecodeError::out_of_range);
  rejects(binary(42, 4, 20).as<weave::i64>(), pg::DecodeError::invalid_value);
  rejects(binary(42, 4, 701).as<int>(), pg::DecodeError::unsupported_type);
  rejects(binary(42, 4, 0).as<int>(), pg::DecodeError::unsupported_type);

  CHECK(binary(std::bit_cast<weave::u32>(1.25F), 4, 700).as<double>() == 1.25);
  CHECK(binary(std::bit_cast<weave::u64>(1.25), 8, 701).as<float>() == 1.25F);
  auto signed_zero = binary(std::bit_cast<weave::u64>(-0.0), 8, 701).as<float>();
  CHECK((signed_zero && std::signbit(*signed_zero)));
  auto nan = binary(std::bit_cast<weave::u64>(std::numeric_limits<double>::quiet_NaN()), 8, 701).as<float>();
  CHECK((nan && std::isnan(*nan)));
  auto infinity = binary(std::bit_cast<weave::u64>(std::numeric_limits<double>::infinity()), 8, 701).as<float>();
  CHECK((infinity && std::isinf(*infinity)));
  rejects(binary(std::bit_cast<weave::u64>(1e100), 8, 701).as<float>(), pg::DecodeError::out_of_range);
  rejects(binary(std::bit_cast<weave::u64>(1e-100), 8, 701).as<float>(), pg::DecodeError::out_of_range);
  rejects(binary(0, 4, 701).as<double>(), pg::DecodeError::invalid_value);
  CHECK(binary(1, 1, 16).as<bool>() == true);
  CHECK(binary(0, 1, 16).as<bool>() == false);
  rejects(binary(2, 1, 16).as<bool>(), pg::DecodeError::invalid_value);
  rejects(binary(1, 1, 17).as<bool>(), pg::DecodeError::unsupported_type);
  rejects(binary(42, 4, 23).as<std::string>(), pg::DecodeError::unsupported_type);
  auto binary_text = text("Ada");
  binary_text.format = pg::Format::binary;
  binary_text.type = 25;
  CHECK(binary_text.as<std::string>() == "Ada");
}

TEST_CASE("PostgreSQL field selectors helpers and readers share checked conversion")
{
  auto result = sample();
  const auto &row = result.rows.front();
  CHECK(row[0].as<weave::i64>() == 42);
  CHECK(row["id"].integer<weave::i64>() == 42);
  CHECK(row.get<double>("weight") == 1.25);
  CHECK(row["weight"].floating<float>() == 1.25F);
  CHECK(row["delivered"].boolean() == true);
  CHECK(row["recipient"].text() == "Ada");
  CHECK(row["recipient"].string() == "Ada");
  auto null = row["notes"].optional<std::string_view>();
  CHECK((null && !*null));
  rejects(row[99].as<int>(), pg::DecodeError::out_of_range);
  rejects(row.get<int>(-1), pg::DecodeError::out_of_range);
  rejects(row["missing"].optional<int>(), pg::DecodeError::missing_column);
  rejects(row["ID"].as<int>(), pg::DecodeError::missing_column);

  auto fields = row.reader();
  auto independent = fields;
  CHECK(fields.remaining() == 5);
  CHECK(fields.next<weave::i64>() == 42);
  CHECK(independent.next().integer<int>() == 42);
  CHECK(fields.next().floating() == 1.25);
  CHECK(fields.next<bool>() == true);
  CHECK(fields.skip(1));
  auto end = fields.next<std::optional<std::string_view>>();
  CHECK((end && !*end));
  CHECK(fields.remaining() == 0);
  rejects(fields.next<int>(), pg::DecodeError::out_of_range);
  rejects(fields.skip(1), pg::DecodeError::out_of_range);
  fields.reset();
  rejects(fields.skip(std::numeric_limits<std::size_t>::max()), pg::DecodeError::out_of_range);
  CHECK(fields.remaining() == 5);
  rejects(fields.next<bool>(), pg::DecodeError::invalid_value);
  CHECK(fields.remaining() == 4);
  CHECK(fields.next<double>() == 1.25);

  auto column = result.column("weight");
  REQUIRE(column);
  if (!column)
    return;
  CHECK(column->index() == 1);
  CHECK(row[*column].as<double>() == 1.25);
  CHECK(row.get<double>(*column) == 1.25);
  auto other = sample();
  rejects(other.rows.front()[*column].as<double>(), pg::DecodeError::schema_mismatch);
  auto copied = result.copy();
  rejects(copied.rows.front()[*column].as<double>(), pg::DecodeError::schema_mismatch);
  auto retained = row;
  auto moved = std::move(result);
  CHECK(moved.rows.front()[*column].as<double>() == 1.25);
  moved = {};
  CHECK(retained["recipient"].text() == "Ada");
  CHECK(retained[*column].as<double>() == 1.25);

  pg::ResultSet ambiguous;
  ambiguous.columns.push_back({.name = "id"});
  ambiguous.columns.push_back({.name = "id"});
  ambiguous.rows.push_back(pg::Row{text("1"), text("2")});
  storage::ResultAccess::bind_rows(ambiguous);
  rejects(ambiguous.column("id"), pg::DecodeError::ambiguous_column);
  rejects(ambiguous.rows.front()["id"].as<int>(), pg::DecodeError::ambiguous_column);
}

TEST_CASE("PostgreSQL wire rows retain primitive metadata after descriptor destruction")
{
  pg::Row retained;
  {
    const std::array columns{pg::Column{.name = "id", .type = 23, .format = pg::Format::binary}};
    storage::Writer body;
    body.integer(1, 2);
    body.integer(4);
    body.integer(42);
    auto decoded = storage::row(body.bytes, columns);
    REQUIRE(decoded);
    if (!decoded)
      return;
    retained = std::move(*decoded);
  }
  CHECK(retained["id"].integer<weave::i64>() == 42);
  CHECK(retained.values().front().type == 23);
}
