#include <weave/postgres/connection.hpp>
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <thread>

namespace pg = weave::pg;

static_assert(std::same_as<decltype(pg::ResultSet{}.column_index("name")), weave::Result<std::optional<std::size_t>>>);

TEST_CASE("PostgreSQL column lookup folds identifiers and preserves quoted names")
{
  pg::ResultSet result;
  const std::array names{"name", "Mixed", "a\"b", "with space", "", "name", "_x1$", "dot.name"};
  for (auto name : names)
    result.columns.push_back({.name = name});

  struct Lookup {
    std::string_view identifier;
    std::optional<std::size_t> index;
  };

  const std::array lookups{
    Lookup{"NAME", 0},
    Lookup{"NaMe", 0},
    Lookup{"\"name\"", 0},
    Lookup{"\"Mixed\"", 1},
    Lookup{"\"a\"\"b\"", 2},
    Lookup{"\"with space\"", 3},
    Lookup{"\"\"", 4},
    Lookup{"_X1$", 6},
    Lookup{"\"dot.name\"", 7},
    Lookup{"mixed", std::nullopt},
    Lookup{"absent", std::nullopt}};

  auto copied = result;
  auto moved = std::move(copied);
  for (const auto &lookup : lookups) {
    auto column = result.column_index(lookup.identifier);
    REQUIRE(column);
    CHECK(*column == lookup.index);
    auto retained = moved.column_index(lookup.identifier);
    REQUIRE(retained);
    CHECK(*retained == lookup.index);
  }

  auto empty = pg::ResultSet{}.column_index("name");
  REQUIRE(empty);
  CHECK_FALSE(*empty);
  CHECK(result.columns[1].name == "Mixed");
  CHECK(result.rows.empty());
  CHECK(result.kind == pg::ResultKind::uninitialized);
}

TEST_CASE("PostgreSQL column lookup rejects malformed input and bounds identifier storage")
{
  pg::ResultSet result;
  result.columns.push_back({.name = "name"});
  const std::array invalid{
    "",
    "\"",
    "\"unfinished",
    "part\"Quoted\"",
    "\"x\"suffix",
    "\"a\"b\"",
    "a b",
    "a.b",
    "a'b",
    "1name",
    "$name",
    "name;",
    " name",
    "name ",
    "a\\b",
    "\"\"\""};
  for (auto identifier : invalid) {
    auto column = result.column_index(identifier);
    REQUIRE_FALSE(column);
    CHECK(column.error() == std::errc::invalid_argument);
  }

  const std::array malformed{
    std::string{"a\0b", 3},
    std::string{"\xc0\x80", 2},
    std::string{"\xed\xa0\x80", 3},
    std::string{"\xf4\x90\x80\x80", 4},
    std::string{"\x80", 1}};
  for (const auto &identifier : malformed) {
    auto column = result.column_index(identifier);
    REQUIRE_FALSE(column);
    CHECK(column.error() == std::errc::illegal_byte_sequence);
    auto quoted = result.column_index("\"" + identifier + "\"");
    REQUIRE_FALSE(quoted);
    CHECK(quoted.error() == std::errc::illegal_byte_sequence);
  }

  auto invalid_encoding = result.column_index("\"\"", static_cast<pg::Encoding>(255));
  REQUIRE_FALSE(invalid_encoding);
  CHECK(invalid_encoding.error() == std::errc::invalid_argument);
  auto exact = result.column_index("name", pg::Encoding::utf8, 4);
  REQUIRE(exact);
  CHECK(*exact == std::optional<std::size_t>{0});
  auto bounded = result.column_index("name", pg::Encoding::utf8, 3);
  REQUIRE_FALSE(bounded);
  CHECK(bounded.error() == pg::make_error_code(pg::Error::resource_limit));
  auto oversized = result.column_index(std::string(65537, 'a'));
  REQUIRE_FALSE(oversized);
  CHECK(oversized.error() == pg::make_error_code(pg::Error::resource_limit));

  result.columns.push_back({.name = std::string(65536, 'a')});
  auto maximum = result.column_index(result.columns.back().name);
  REQUIRE(maximum);
  CHECK(*maximum == std::optional<std::size_t>{1});
  auto quoted_maximum = result.column_index("\"" + result.columns.back().name + "\"", pg::Encoding::utf8, 65538);
  REQUIRE(quoted_maximum);
  CHECK(*quoted_maximum == std::optional<std::size_t>{1});
}

TEST_CASE("PostgreSQL column lookup preserves complete characters in all client encodings")
{
  const std::array samples{
    std::pair{pg::Encoding::euc_jp, std::string{"\xa1\xa1"}},
    std::pair{pg::Encoding::euc_cn, std::string{"\xa1\xa1"}},
    std::pair{pg::Encoding::euc_kr, std::string{"\xa1\xa1"}},
    std::pair{pg::Encoding::euc_tw, std::string{"\x8e\xa1\xa1\xa1"}},
    std::pair{pg::Encoding::euc_jis_2004, std::string{"\xa1\xa1"}},
    std::pair{pg::Encoding::utf8, std::string{"\xc3\xa9"}},
    std::pair{pg::Encoding::mule_internal, std::string{"\x81\xa1"}},
    std::pair{pg::Encoding::sjis, std::string{"\x83\x41"}},
    std::pair{pg::Encoding::big5, std::string{"\x81\x41"}},
    std::pair{pg::Encoding::gbk, std::string{"\x81\x41"}},
    std::pair{pg::Encoding::uhc, std::string{"\x81\x41"}},
    std::pair{pg::Encoding::gb18030, std::string{"\x81\x30\x81\x30"}},
    std::pair{pg::Encoding::johab, std::string{"\xa1\xa1"}},
    std::pair{pg::Encoding::shift_jis_2004, std::string{"\x83\x41"}}};

  for (unsigned value = 0; value < 42; ++value) {
    auto encoding = static_cast<pg::Encoding>(value);
    std::string character{"\xc0"};
    for (const auto &[candidate, text] : samples) {
      if (candidate == encoding)
        character = text;
    }
    CAPTURE(value);
    REQUIRE(pg::validate_text(character, encoding));
    pg::ResultSet result;
    result.columns.push_back({.name = character + "name"});

    auto column = result.column_index(character + "NAME", encoding);
    REQUIRE(column);
    CHECK(*column == std::optional<std::size_t>{0});
    auto identifier = pg::escape_identifier(result.columns[0].name, encoding);
    REQUIRE(identifier);
    auto quoted = result.column_index(*identifier, encoding);
    REQUIRE(quoted);
    CHECK(*quoted == std::optional<std::size_t>{0});
  }
}

TEST_CASE("PostgreSQL immutable column lookup has no ambient execution or mutable parser state")
{
  pg::ResultSet result;
  result.columns.push_back({.name = "Mixed"});
  result.columns.push_back({.name = "name"});
  std::atomic<bool> valid{true};
  std::vector<std::jthread> threads;
  for (unsigned worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&] {
      for (unsigned iteration = 0; iteration < 10000; ++iteration) {
        auto quoted = result.column_index("\"Mixed\"");
        auto folded = result.column_index("NAME");
        auto absent = result.column_index("mixed");
        if (!quoted || *quoted != std::optional<std::size_t>{0} || !folded ||
          *folded != std::optional<std::size_t>{1} || !absent || *absent) {
          valid.store(false);
          return;
        }
      }
    });
  }
  threads.clear();
  CHECK(valid.load());
}
