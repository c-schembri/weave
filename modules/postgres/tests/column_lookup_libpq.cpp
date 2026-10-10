#include <weave/postgres/connection.hpp>
#include <libpq-fe.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <clocale>
#include <type_traits>

namespace pg = weave::pg;
static unsigned checks = 0;

static void check(bool value)
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "check %u failed\n", checks);
    std::exit(EXIT_FAILURE);
  }
}

static void found(
  const pg::ResultSet &result,
  std::string_view name,
  std::size_t index,
  pg::Encoding encoding = pg::Encoding::utf8)
{
  auto column = result.column_index(name, encoding);
  check(column && *column && **column == index);
}

static PGresult *native(const pg::ResultSet &result)
{
  auto handle = PQmakeEmptyPGresult(nullptr, PGRES_TUPLES_OK);
  check(handle != nullptr);
  std::vector<PGresAttDesc> attributes(result.columns.size());
  for (std::size_t index = 0; index < attributes.size(); ++index) {
    attributes[index].name = const_cast<char *>(result.columns[index].name.c_str());
    attributes[index].typid = 25;
    attributes[index].typlen = -1;
  }
  check(PQsetResultAttrs(handle, static_cast<int>(attributes.size()), attributes.data()) == 1);
  return handle;
}

int main()
{
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  check(std::setlocale(LC_CTYPE, "C") != nullptr);
  static_assert(
    std::same_as<decltype(pg::ResultSet{}.column_index("name")), weave::Result<std::optional<std::size_t>>>);

  pg::ResultSet result;
  const std::array names{"name", "Mixed", "a\"b", "with space", "", "name", "_x1$", "dot.name"};
  for (auto name : names)
    result.columns.push_back({.name = name});
  auto handle = native(result);

  struct Lookup {
    const char *name;
    int index;
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
    Lookup{"mixed", -1},
    Lookup{"absent", -1}};
  for (auto lookup : lookups) {
    check(PQfnumber(handle, lookup.name) == lookup.index);
    auto index = result.column_index(lookup.name);
    check(index.has_value());
    check(lookup.index < 0 ? !*index : *index == static_cast<std::size_t>(lookup.index));
  }
  PQclear(handle);

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
  for (auto name : invalid) {
    auto index = result.column_index(name);
    check(!index && index.error() == std::errc::invalid_argument);
  }
  const std::array malformed{
    std::string{"a\0b", 3},
    std::string{"\xc0\x80", 2},
    std::string{"\xed\xa0\x80", 3},
    std::string{"\xf4\x90\x80\x80", 4},
    std::string{"\x80", 1}};
  for (const auto &name : malformed) {
    auto index = result.column_index(name);
    check(!index && index.error() == std::errc::illegal_byte_sequence);
    auto quoted = result.column_index("\"" + name + "\"");
    check(!quoted && quoted.error() == std::errc::illegal_byte_sequence);
  }
  check(!result.column_index("name", static_cast<pg::Encoding>(255)));
  check(!result.column_index("\"\"", static_cast<pg::Encoding>(255)));
  auto exact = result.column_index("name", pg::Encoding::utf8, 4);
  check(exact && *exact == std::optional<std::size_t>{0});
  check(result.column_index("name", pg::Encoding::utf8, 3).error() == pg::make_error_code(pg::Error::resource_limit));
  result.columns.push_back({.name = std::string(65536, 'a')});
  found(result, result.columns.back().name, 8);
  check(result.column_index(std::string(65537, 'a')).error() == pg::make_error_code(pg::Error::resource_limit));
  check(result.column_index("\"\"", pg::Encoding::utf8, 1).error() == pg::make_error_code(pg::Error::resource_limit));

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
    auto info = pg::encoding_info(encoding);
    check(info.has_value());
    std::string character{"\xc0"};
    for (const auto &[candidate, text] : samples) {
      if (candidate == encoding)
        character = text;
    }
    check(pg::validate_text(character, encoding).has_value());
    pg::ResultSet legacy;
    legacy.columns.push_back({.name = character + "name"});
    found(legacy, character + "NAME", 0, encoding);
    found(legacy, "\"" + character + "name\"", 0, encoding);
    auto quote = pg::escape_identifier(character + "name", encoding);
    check(quote.has_value());
    found(legacy, *quote, 0, encoding);
    handle = native(legacy);
    check(PQfnumber(handle, quote->c_str()) == 0);
    const bool ascii_continuation = encoding == pg::Encoding::sjis || encoding == pg::Encoding::big5 ||
      encoding == pg::Encoding::gbk || encoding == pg::Encoding::uhc || encoding == pg::Encoding::shift_jis_2004;
    auto folded = character + "NAME";
    check(PQfnumber(handle, folded.c_str()) == (ascii_continuation ? -1 : 0));
    PQclear(handle);
    if (info->max_bytes > 1) {
      auto bad = legacy.column_index(character.substr(0, 1), encoding);
      check(!bad && bad.error() == std::errc::illegal_byte_sequence);
    }
  }

  // Independent native controls for generated identifiers and quoted arbitrary names.
  unsigned random = 0x12345678;
  const std::string alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789$";
  const std::string quoted_alphabet = "aAZ09$_ .\"\n\\';";
  for (unsigned iteration = 0; iteration < 20000; ++iteration) {
    std::string input = "X";
    std::string folded = "x";
    for (unsigned index = 0; index < iteration % 63; ++index) {
      random = random * 1664525 + 1013904223;
      char byte = alphabet[random % alphabet.size()];
      input.push_back(byte);
      folded.push_back(byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + 32) : byte);
    }
    pg::ResultSet generated;
    generated.columns.push_back({.name = folded});
    generated.columns.push_back({.name = folded});
    handle = native(generated);
    found(generated, input, 0);
    check(PQfnumber(handle, input.c_str()) == 0);
    auto copied = generated;
    auto moved = std::move(copied);
    found(moved, input, 0);
    check(!generated.column_index(input + "missing")->has_value());
    PQclear(handle);

    std::string name;
    for (unsigned index = 0; index < iteration % 31; ++index) {
      random = random * 1664525 + 1013904223;
      name.push_back(quoted_alphabet[random % quoted_alphabet.size()]);
    }
    auto quoted = pg::escape_identifier(name);
    check(quoted.has_value());
    generated.columns[0].name = name;
    handle = native(generated);
    found(generated, *quoted, 0);
    check(PQfnumber(handle, quoted->c_str()) == 0);
    PQclear(handle);
  }

  // libpq permits partial quoting; Weave deliberately rejects it.
  pg::ResultSet strict;
  strict.columns.push_back({.name = "fooBARfoo"});
  handle = native(strict);
  check(PQfnumber(handle, "foo\"BAR\"foo") == 0);
  check(strict.column_index("foo\"BAR\"foo").error() == std::errc::invalid_argument);
  found(strict, "\"fooBARfoo\"", 0);
  PQclear(handle);
  std::printf("%u checks; native libpq %d; no sockets or Context\n", checks, PQlibVersion());
}
