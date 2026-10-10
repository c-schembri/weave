#pragma once

#include <weave/core.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace weave::pg {

enum class Encoding : u8 {
  sql_ascii,
  euc_jp,
  euc_cn,
  euc_kr,
  euc_tw,
  euc_jis_2004,
  utf8,
  mule_internal,
  latin1,
  latin2,
  latin3,
  latin4,
  latin5,
  latin6,
  latin7,
  latin8,
  latin9,
  latin10,
  win1256,
  win1258,
  win866,
  win874,
  koi8r,
  win1251,
  win1252,
  iso_8859_5,
  iso_8859_6,
  iso_8859_7,
  iso_8859_8,
  win1250,
  win1253,
  win1254,
  win1255,
  win1257,
  koi8u,
  sjis,
  big5,
  gbk,
  uhc,
  gb18030,
  johab,
  shift_jis_2004,
};

struct EncodingInfo {
  std::string_view name;
  u8 max_bytes;
  bool server;
};

Result<Encoding> parse_encoding(std::string_view name) noexcept;
Result<EncodingInfo> encoding_info(Encoding encoding) noexcept;
Result<std::size_t> character_size(std::string_view text, Encoding encoding = Encoding::utf8) noexcept;
Result<i32> character_width(std::string_view text, Encoding encoding = Encoding::utf8) noexcept;
Result<void> validate_text(std::string_view text, Encoding encoding = Encoding::utf8) noexcept;
Result<std::string> escape_literal(
  std::string_view text,
  Encoding encoding = Encoding::utf8,
  std::size_t limit = 16 * 1024 * 1024);
Result<std::string> escape_identifier(
  std::string_view text,
  Encoding encoding = Encoding::utf8,
  std::size_t limit = 16 * 1024 * 1024);

Result<std::string> encode_bytea(std::span<const std::byte> bytes, std::size_t limit = 16 * 1024 * 1024);
Result<std::vector<std::byte>> decode_bytea(std::string_view text, std::size_t limit = 16 * 1024 * 1024);

} // namespace weave::pg
