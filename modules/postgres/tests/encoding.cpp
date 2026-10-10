#include <weave/postgres/encoding.hpp>
#include <weave/postgres/connection.hpp>
#include <doctest/doctest.h>
#include <array>
#include <limits>

namespace pg = weave::pg;

TEST_CASE("PostgreSQL encodings have bounded canonical metadata and documented aliases")
{
  const std::array names{
    "SQL_ASCII",
    "EUC_JP",
    "EUC_CN",
    "EUC_KR",
    "EUC_TW",
    "EUC_JIS_2004",
    "UTF8",
    "MULE_INTERNAL",
    "LATIN1",
    "LATIN2",
    "LATIN3",
    "LATIN4",
    "LATIN5",
    "LATIN6",
    "LATIN7",
    "LATIN8",
    "LATIN9",
    "LATIN10",
    "WIN1256",
    "WIN1258",
    "WIN866",
    "WIN874",
    "KOI8R",
    "WIN1251",
    "WIN1252",
    "ISO_8859_5",
    "ISO_8859_6",
    "ISO_8859_7",
    "ISO_8859_8",
    "WIN1250",
    "WIN1253",
    "WIN1254",
    "WIN1255",
    "WIN1257",
    "KOI8U",
    "SJIS",
    "BIG5",
    "GBK",
    "UHC",
    "GB18030",
    "JOHAB",
    "SHIFT_JIS_2004"};
  for (std::size_t index = 0; index < names.size(); ++index) {
    auto encoding = pg::parse_encoding(names[index]);
    REQUIRE(encoding);
    auto info = pg::encoding_info(*encoding);
    REQUIRE(info);
    CHECK(info->name == names[index]);
    CHECK(info->max_bytes >= 1);
    CHECK(info->max_bytes <= 4);
    CHECK(info->server == (index < 35));
    CHECK(pg::validate_text("ASCII", *encoding).has_value());
    CHECK(pg::character_size("", *encoding) == 0);
    CHECK(pg::character_width("", *encoding) == 0);
  }

  struct Alias {
    std::string_view name;
    pg::Encoding encoding;
  };

  const std::array aliases{
    Alias{"unicode", pg::Encoding::utf8},
    Alias{"uTf-8", pg::Encoding::utf8},
    Alias{"ISO-8859-1", pg::Encoding::latin1},
    Alias{"ISO885916", pg::Encoding::latin10},
    Alias{"Windows1252", pg::Encoding::win1252},
    Alias{"Windows932", pg::Encoding::sjis},
    Alias{"Mskanji", pg::Encoding::sjis},
    Alias{"ShiftJIS", pg::Encoding::sjis},
    Alias{"Shift-JIS-2004", pg::Encoding::shift_jis_2004},
    Alias{"WIN950", pg::Encoding::big5},
    Alias{"WIN936", pg::Encoding::gbk},
    Alias{"WIN949", pg::Encoding::uhc},
    Alias{"TCVN5712", pg::Encoding::win1258},
    Alias{"ALT", pg::Encoding::win866},
    Alias{"WIN", pg::Encoding::win1251},
    Alias{"KOI8", pg::Encoding::koi8r}};
  for (const auto &alias : aliases)
    CHECK(pg::parse_encoding(alias.name) == alias.encoding);

  const std::array invalid{
    std::string{},
    std::string{"..."},
    std::string{"utf16"},
    std::string{"UTF8\0LATIN1", 11},
    std::string{"\xff", 1},
    std::string(64, 'a')};
  for (const auto &name : invalid) {
    auto encoding = pg::parse_encoding(name);
    REQUIRE_FALSE(encoding);
    CHECK(encoding.error() == std::errc::invalid_argument);
  }
  auto invalid_encoding = static_cast<pg::Encoding>(255);
  CHECK_FALSE(pg::encoding_info(invalid_encoding));
  CHECK_FALSE(pg::character_size("", invalid_encoding));
  CHECK_FALSE(pg::character_width("", invalid_encoding));
  CHECK_FALSE(pg::validate_text("", invalid_encoding));
  CHECK_FALSE(pg::escape_literal("", invalid_encoding));
  CHECK_FALSE(pg::escape_identifier("", invalid_encoding));
}

TEST_CASE("PostgreSQL multibyte quoting preserves continuation backslashes and owns complete output")
{
  struct Sample {
    pg::Encoding encoding;
    std::string_view text;
    std::size_t first_size;
  };

  const std::array samples{
    Sample{pg::Encoding::utf8, "\xc3\xa9", 2},
    Sample{pg::Encoding::euc_jp, "\x8e\xa1", 2},
    Sample{pg::Encoding::euc_jp, "\x8f\xa1\xa1", 3},
    Sample{pg::Encoding::euc_cn, "\xa1\xa1", 2},
    Sample{pg::Encoding::euc_kr, "\xa1\xa1", 2},
    Sample{pg::Encoding::euc_tw, "\x8e\xa2\xa1\xa1", 4},
    Sample{pg::Encoding::euc_jis_2004, "\x8e\xa1", 2},
    Sample{pg::Encoding::mule_internal, "\x81\xa1", 2},
    Sample{pg::Encoding::mule_internal, "\x90\xa1\xa1", 3},
    Sample{pg::Encoding::mule_internal, "\x9c\xa1\xa1\xa1", 4},
    Sample{pg::Encoding::sjis, "\x83\x5c", 2},
    Sample{pg::Encoding::shift_jis_2004, "\x83\x5c", 2},
    Sample{pg::Encoding::sjis, "\xa1", 1},
    Sample{pg::Encoding::big5, "\xa5\x5c", 2},
    Sample{pg::Encoding::gbk, "\x81\x5c", 2},
    Sample{pg::Encoding::uhc, "\x81\x61", 2},
    Sample{pg::Encoding::gb18030, "\x90\x30\x81\x30", 4},
    Sample{pg::Encoding::johab, "\xd0\xd0", 2},
    Sample{pg::Encoding::latin1, "\xe9", 1}};
  for (const auto &sample : samples) {
    CAPTURE(sample.encoding);
    CHECK(pg::character_size(sample.text, sample.encoding) == sample.first_size);
    CHECK(pg::validate_text(sample.text, sample.encoding).has_value());
    auto text = std::string{sample.text} + "'\\\"";
    auto literal = "E'" + std::string{sample.text} + "''\\\\\"'";
    auto identifier = "\"" + std::string{sample.text} + "'\\\"\"\"";
    CHECK(pg::escape_literal(text, sample.encoding, literal.size()) == literal);
    CHECK(pg::escape_identifier(text, sample.encoding, identifier.size()) == identifier);
    auto limited = pg::escape_literal(text, sample.encoding, literal.size() - 1);
    REQUIRE_FALSE(limited);
    CHECK(limited.error() == pg::Error::resource_limit);
    CHECK_FALSE(pg::escape_identifier(text, sample.encoding, identifier.size() - 1));
    CHECK_FALSE(pg::validate_text(std::string{sample.text} + std::string{"\0", 1}, sample.encoding));
    if (sample.first_size > 1)
      CHECK_FALSE(pg::validate_text(sample.text.substr(0, sample.first_size - 1), sample.encoding));
  }
  CHECK(pg::escape_literal("x", pg::Encoding::utf8, std::numeric_limits<std::size_t>::max()) == "E'x'");
}

TEST_CASE("PostgreSQL legacy framing validation is not a converter or permissive quoting policy")
{
  const std::array permissive{pg::Encoding::big5, pg::Encoding::gbk, pg::Encoding::uhc};
  const std::array syntax{std::string{"\x81'", 2}, std::string{"\x81\"", 2}};
  for (auto encoding : permissive) {
    for (const auto &text : syntax) {
      CHECK(pg::validate_text(text, encoding).has_value());
      auto quote = pg::escape_literal(text, encoding);
      REQUIRE_FALSE(quote);
      CHECK(quote.error() == std::errc::illegal_byte_sequence);
      CHECK_FALSE(pg::escape_identifier(text, encoding));
    }
    CHECK_FALSE(pg::validate_text("\x8d\x20", encoding));
    CHECK_FALSE(pg::validate_text(std::string{"\x81\0", 2}, encoding));
  }

  struct Invalid {
    pg::Encoding encoding;
    std::string_view text;
  };

  const std::array invalid{
    Invalid{pg::Encoding::sjis, "\x81\x7f"},
    Invalid{pg::Encoding::sjis, "\xff"},
    Invalid{pg::Encoding::euc_jp, "\x8e\xe0"},
    Invalid{pg::Encoding::euc_cn, "\x8f\xa1\xa1"},
    Invalid{pg::Encoding::euc_tw, "\x8e\xa8\xa1\xa1"},
    Invalid{pg::Encoding::gb18030, "\x81\x30\x81\x2f"},
    Invalid{pg::Encoding::mule_internal, "\x81\x7f"},
    Invalid{pg::Encoding::johab, "\xd0\x5c"}};
  for (const auto &sample : invalid) {
    CHECK_FALSE(pg::validate_text(sample.text, sample.encoding));
    CHECK_FALSE(pg::escape_literal(sample.text, sample.encoding));
    CHECK_FALSE(pg::escape_identifier(sample.text, sample.encoding));
  }
}

TEST_CASE("PostgreSQL display widths distinguish Unicode code points from bytes and graphemes")
{
  CHECK(pg::character_width("A") == 1);
  CHECK(pg::character_width("\t") == -1);
  CHECK(pg::character_width("\xc2\x85") == -1);
  CHECK(pg::character_width("\xcc\x81") == 0);
  CHECK(pg::character_width("\xe2\x80\x8d") == 0);
  CHECK(pg::character_width("\xe1\x85\xa0") == 0);
  CHECK(pg::character_width("\xe4\xb8\xad") == 2);
  CHECK(pg::character_width("\xf0\x9f\x98\x80") == 2);
  CHECK(pg::character_width("\xc3\xa9") == 1);
  CHECK(pg::character_width("\x8e\xa1", pg::Encoding::euc_jp) == 1);
  CHECK(pg::character_width("\x83\x5c", pg::Encoding::sjis) == 2);
  CHECK(pg::character_width("\t", pg::Encoding::mule_internal) == 1);
  CHECK_FALSE(pg::character_width("\x80"));
}
