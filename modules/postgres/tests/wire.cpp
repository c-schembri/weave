#include "wire.hpp"
#include <weave/postgres/encoding.hpp>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <random>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;

TEST_CASE("PostgreSQL quoting validates UTF8 and bounds fully escaped output")
{
  CHECK(pg::escape_literal("a'b\\c", pg::Encoding::utf8, 20) == "E'a''b\\\\c'");
  CHECK(pg::escape_identifier("a\"b\\c", pg::Encoding::utf8, 20) == "\"a\"\"b\\c\"");
  CHECK(pg::escape_literal("", pg::Encoding::utf8, 3) == "E''");
  CHECK(pg::escape_identifier("", pg::Encoding::utf8, 2) == "\"\"");
  CHECK_FALSE(pg::escape_literal("'", pg::Encoding::utf8, 4));
  CHECK(pg::escape_literal("'", pg::Encoding::utf8, 5) == "E''''");

  const std::array invalid{
    std::string{"\0", 1},
    std::string{"\xc0\xaf", 2},
    std::string{"\xed\xa0\x80", 3},
    std::string{"\xf4\x90\x80\x80", 4},
    std::string{"\xe2\x82", 2},
    std::string{"\x80", 1}};
  for (const auto &text : invalid) {
    auto literal = pg::escape_literal(text, pg::Encoding::utf8, 64);
    auto identifier = pg::escape_identifier(text, pg::Encoding::utf8, 64);
    REQUIRE_FALSE(literal);
    REQUIRE_FALSE(identifier);
    CHECK(literal.error() == std::errc::illegal_byte_sequence);
    CHECK(identifier.error() == std::errc::illegal_byte_sequence);
  }

  std::string unicode{"\xc3\xa9\xf0\x9f\x98\x80", 6};
  CHECK(pg::escape_literal(unicode, pg::Encoding::utf8, 9) == "E'" + unicode + "'");
}

TEST_CASE("PostgreSQL bytea owns exact bytes and rejects malformed or oversized input")
{
  std::vector<std::byte> bytes;
  for (unsigned value = 0; value < 256; ++value)
    bytes.push_back(static_cast<std::byte>(value));

  auto encoded = pg::encode_bytea(bytes, 514);
  REQUIRE(encoded);
  CHECK(pg::decode_bytea(*encoded, 256) == bytes);
  CHECK_FALSE(pg::encode_bytea(bytes, 513));
  CHECK_FALSE(pg::decode_bytea(*encoded, 255));
  CHECK(pg::encode_bytea({}, 2) == "\\x");
  CHECK(pg::decode_bytea("\\x", 0) == std::vector<std::byte>{});

  std::vector<std::byte> legacy{std::byte{'a'}, std::byte{0}, std::byte{255}, std::byte{'\\'}};
  CHECK(pg::decode_bytea("a\\000\\377\\\\") == legacy);
  std::vector<std::byte> hex{std::byte{0xab}, std::byte{0xcd}};
  CHECK(pg::decode_bytea("\\x AB\tcd \n") == hex);

  const std::array malformed{"\\x0", "\\x0 1", "\\xgg", "\\", "\\0", "\\008", "\\400", "\\q"};
  for (auto text : malformed)
    CHECK_FALSE(pg::decode_bytea(text));
}

TEST_CASE("PostgreSQL function calls preserve mixed formats and validate the complete packet size")
{
  std::vector<pg::Parameter> parameters{{"42"}, {std::nullopt}, {std::string{"\0*", 2}, 0, pg::Format::binary}};
  auto request = wire::function_call(551, parameters, pg::Format::binary, 37);
  REQUIRE(request);
  CHECK(request->bytes.size() == 37);
  CHECK_FALSE(wire::function_call(551, parameters, pg::Format::binary, 36));

  wire::Reader reader{request->bytes};
  CHECK(reader.integer(1) == 'F');
  CHECK(reader.integer() == 36);
  CHECK(reader.integer() == 551);
  CHECK(reader.integer(2) == 3);
  CHECK(reader.integer(2) == 0);
  CHECK(reader.integer(2) == 0);
  CHECK(reader.integer(2) == 1);
  CHECK(reader.integer(2) == 3);
  CHECK(reader.integer() == 2);
  CHECK(reader.integer(2) == 0x3432);
  CHECK(reader.integer() == 0xffffffff);
  CHECK(reader.integer() == 2);
  CHECK(reader.integer(2) == 42);
  CHECK(reader.integer(2) == 1);
  CHECK(reader.empty());

  CHECK_FALSE(wire::function_call(0, {}, pg::Format::text, 64));
  parameters.front().type = 23;
  CHECK_FALSE(wire::function_call(551, parameters, pg::Format::binary, 64));
  parameters.front().type = 0;
  parameters.front().data = std::string{"\0", 1};
  CHECK_FALSE(wire::function_call(551, parameters, pg::Format::binary, 64));
}

TEST_CASE("PostgreSQL integer and string framing reject every truncated prefix")
{
  wire::Writer message;
  message.integer(0x01234567);
  message.integer(0x89ab, 2);
  message.string("hello");

  wire::Reader reader{message.bytes};
  CHECK(reader.integer() == 0x01234567);
  CHECK(reader.integer(2) == 0x89ab);
  CHECK(reader.string() == "hello");
  CHECK(reader.empty());

  for (std::size_t size = 0; size < message.bytes.size(); ++size) {
    wire::Reader truncated{std::span{message.bytes}.first(size)};
    truncated.integer();
    truncated.integer(2);
    truncated.string();
    CHECK_FALSE(truncated.empty());
  }
}

TEST_CASE("PostgreSQL rows preserve binary bytes NULL and empty values")
{
  wire::Writer message;
  message.integer(3, 2);
  message.integer(0xffffffff);
  message.integer(0);
  message.integer(4);
  message.integer(42);
  auto row = wire::row(message.bytes, 3);
  REQUIRE(row);
  REQUIRE(row->size() == 3);
  CHECK((*row)[0].is_null());
  CHECK_FALSE((*row)[1].is_null());
  CHECK((*row)[1].bytes().empty());
  CHECK((*row)[2].bytes() == std::string_view{"\0\0\0*", 4});
  CHECK_FALSE(wire::row(message.bytes, 2));

  pg::Value binary{std::string{"\0\0\0*", 4}, pg::Format::binary};
  CHECK(binary.binary_integer<weave::i32>() == 42);
  CHECK_FALSE(binary.integer<int>());
  binary.data = std::string(4, static_cast<char>(0xff));
  CHECK(binary.binary_integer<weave::i32>() == -1);
  CHECK_FALSE(binary.binary_integer<weave::i64>());

  for (std::size_t size = 0; size < message.bytes.size(); ++size)
    CHECK_FALSE(wire::row(std::span{message.bytes}.first(size), 3));
}

TEST_CASE("PostgreSQL diagnostics SQLSTATE and command counts own their data")
{
  wire::Writer message;
  message.integer('C', 1);
  message.string("23505");
  message.integer('M', 1);
  message.string("duplicate key");
  message.integer(0, 1);
  auto diagnostic = wire::diagnostic(message.bytes);
  REQUIRE(diagnostic);
  CHECK(diagnostic->message() == "duplicate key");
  CHECK(diagnostic->sqlstate() == "23505");
  CHECK(pg::sqlstate(pg::sql_error("23505")) == "23505");
  CHECK(pg::sql_error("00000") == pg::Error::protocol);
  CHECK(pg::sql_error("2350") == pg::Error::protocol);
  CHECK(pg::sql_error("abcde") == pg::Error::protocol);

  const std::vector<std::pair<std::string, weave::u64>>
    commands{{"CREATE TABLE", 0}, {"DROP TABLE", 0}, {"INSERT 0 2", 2}, {"COPY 1024", 1024}, {"SELECT 0", 0}};
  for (const auto &[command, count] : commands) {
    pg::ResultSet result;
    result.command = command;
    CHECK(result.affected_rows() == count);
  }

  message.bytes.pop_back();
  message.integer('M', 1);
  message.string("");
  message.integer(0, 1);
  CHECK_FALSE(wire::diagnostic(message.bytes));
}

TEST_CASE("PostgreSQL inserted OID accessors preserve zero and reject invalid numeric tags")
{
  struct Case {
    const char *tag;
    weave::u32 oid;
    const char *text;
  };

  const std::array cases{
    Case{"INSERT 0 1", 0, "0"},
    Case{"INSERT 123 2", 123, "123"},
    Case{"INSERT 4294967295 1", 4294967295u, "4294967295"},
    Case{"INSERT 00012 1", 12, "00012"},
    Case{"INSERT 12", 12, "12"},
    Case{"UPDATE 12", 0, ""},
    Case{"", 0, ""}};
  for (const auto &scenario : cases) {
    pg::ResultSet result;
    result.command = scenario.tag;
    CHECK(result.inserted_oid() == scenario.oid);
    CHECK(result.inserted_oid_text() == std::string_view{scenario.text});
    auto copy = result.copy();
    CHECK(copy.inserted_oid() == scenario.oid);
    CHECK(copy.inserted_oid_text() == std::string_view{scenario.text});
  }
  const std::array malformed{"INSERT ", "INSERT -1 1", "INSERT +1 1", "INSERT 12x 1", "INSERT 4294967296 1"};
  for (const auto *tag : malformed) {
    pg::ResultSet result;
    result.command = tag;
    auto oid = result.inserted_oid();
    auto text = result.inserted_oid_text();
    CHECK((!oid && oid.error() == std::errc::invalid_argument));
    CHECK((!text && text.error() == std::errc::invalid_argument));
  }
}

TEST_CASE("PostgreSQL parameter encoding validates formats counts and bounds before allocation")
{
  wire::Writer request;
  std::vector<pg::Parameter> parameters{
    {"42", 23},
    {std::nullopt, 25},
    {std::string{"\0x", 2}, 17, pg::Format::binary}};
  CHECK(wire::bind(request, "statement", parameters, pg::Format::binary, 1024));
  CHECK_FALSE(wire::bind(request, "statement", parameters, pg::Format::binary, 16));

  parameters.back().format = pg::Format::text;
  CHECK_FALSE(wire::bind(request, "statement", parameters, pg::Format::text, 1024));
  parameters.back().data = "valid";
  parameters.back().format = static_cast<pg::Format>(2);
  CHECK_FALSE(wire::bind(request, "statement", parameters, pg::Format::text, 1024));
  parameters.assign(65536, {});
  CHECK_FALSE(wire::bind(request, "", parameters, pg::Format::text, 1024));
}

TEST_CASE("PostgreSQL hostile message bodies are bounded and never read out of range")
{
  std::minstd_rand random{20261007};
  for (std::size_t iteration = 0; iteration < 4096; ++iteration) {
    wire::Bytes bytes(iteration % 257);
    for (auto &byte : bytes)
      byte = static_cast<std::byte>(random() & 255);

    auto columns = wire::columns(bytes);
    auto row = wire::row(bytes, iteration % 8);
    auto diagnostic = wire::diagnostic(bytes);
    auto notification = wire::notification(bytes);
    if (columns) {
      for (const auto &column : *columns)
        CHECK((column.format == pg::Format::text || column.format == pg::Format::binary));
    }
    if (row)
      CHECK(row->size() == iteration % 8);
    if (diagnostic)
      CHECK(diagnostic->fields.size() <= bytes.size());
    if (notification)
      CHECK(notification->channel.size() + notification->payload.size() + 6 == bytes.size());
  }
}
