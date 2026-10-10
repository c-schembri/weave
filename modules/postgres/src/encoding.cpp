#include <weave/postgres/encoding.hpp>
#include <weave/postgres/connection.hpp>
#include <unicode/utf8.h>
#include <unicode/uchar.h>
#include <array>
#include <algorithm>

namespace weave::pg {

namespace {

constexpr std::array<EncodingInfo, 42> encodings{{
  {"SQL_ASCII", 1, true},
  {"EUC_JP", 3, true},
  {"EUC_CN", 3, true},
  {"EUC_KR", 3, true},
  {"EUC_TW", 4, true},
  {"EUC_JIS_2004", 3, true},
  {"UTF8", 4, true},
  {"MULE_INTERNAL", 4, true},
  {"LATIN1", 1, true},
  {"LATIN2", 1, true},
  {"LATIN3", 1, true},
  {"LATIN4", 1, true},
  {"LATIN5", 1, true},
  {"LATIN6", 1, true},
  {"LATIN7", 1, true},
  {"LATIN8", 1, true},
  {"LATIN9", 1, true},
  {"LATIN10", 1, true},
  {"WIN1256", 1, true},
  {"WIN1258", 1, true},
  {"WIN866", 1, true},
  {"WIN874", 1, true},
  {"KOI8R", 1, true},
  {"WIN1251", 1, true},
  {"WIN1252", 1, true},
  {"ISO_8859_5", 1, true},
  {"ISO_8859_6", 1, true},
  {"ISO_8859_7", 1, true},
  {"ISO_8859_8", 1, true},
  {"WIN1250", 1, true},
  {"WIN1253", 1, true},
  {"WIN1254", 1, true},
  {"WIN1255", 1, true},
  {"WIN1257", 1, true},
  {"KOI8U", 1, true},
  {"SJIS", 2, false},
  {"BIG5", 2, false},
  {"GBK", 2, false},
  {"UHC", 2, false},
  {"GB18030", 4, false},
  {"JOHAB", 3, false},
  {"SHIFT_JIS_2004", 2, false},
}};

struct EncodingAlias {
  std::string_view name;
  Encoding encoding;
};

constexpr std::array aliases{
  EncodingAlias{"iso88591", Encoding::latin1},
  EncodingAlias{"iso88592", Encoding::latin2},
  EncodingAlias{"iso88593", Encoding::latin3},
  EncodingAlias{"iso88594", Encoding::latin4},
  EncodingAlias{"iso88599", Encoding::latin5},
  EncodingAlias{"iso885910", Encoding::latin6},
  EncodingAlias{"iso885913", Encoding::latin7},
  EncodingAlias{"iso885914", Encoding::latin8},
  EncodingAlias{"iso885915", Encoding::latin9},
  EncodingAlias{"iso885916", Encoding::latin10},
  EncodingAlias{"abc", Encoding::win1258},
  EncodingAlias{"tcvn", Encoding::win1258},
  EncodingAlias{"tcvn5712", Encoding::win1258},
  EncodingAlias{"vscii", Encoding::win1258},
  EncodingAlias{"alt", Encoding::win866},
  EncodingAlias{"koi8", Encoding::koi8r},
  EncodingAlias{"mskanji", Encoding::sjis},
  EncodingAlias{"shiftjis", Encoding::sjis},
  EncodingAlias{"unicode", Encoding::utf8},
  EncodingAlias{"win", Encoding::win1251},
  EncodingAlias{"win932", Encoding::sjis},
  EncodingAlias{"win936", Encoding::gbk},
  EncodingAlias{"win949", Encoding::uhc},
  EncodingAlias{"win950", Encoding::big5},
};

Result<std::size_t> invalid_character() noexcept
{
  return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
}

bool in_range(u8 byte, u8 first, u8 last) noexcept
{
  return byte >= first && byte <= last;
}

bool encoding_name_equal(std::string_view key, std::string_view name) noexcept
{
  std::size_t offset = 0;
  for (char byte : name) {
    if (byte == '_')
      continue;
    if (byte >= 'A' && byte <= 'Z')
      byte = static_cast<char>(byte - 'A' + 'a');
    if (offset == key.size() || key[offset++] != byte)
      return false;
  }
  return offset == key.size();
}

Result<std::size_t> encoded_character(std::string_view text, Encoding encoding) noexcept
{
  if (text.empty())
    return std::size_t{0};

  auto first = static_cast<u8>(text.front());
  if (!first)
    return invalid_character();
  if (first < 0x80 || encodings[static_cast<std::size_t>(encoding)].max_bytes == 1)
    return std::size_t{1};

  if (encoding == Encoding::utf8) {
    int32_t offset = 0;
    auto length = static_cast<int32_t>(std::min(text.size(), std::size_t{4}));
    UChar32 value = 0;
    U8_NEXT(text.data(), offset, length, value);
    if (value < 0)
      return invalid_character();
    return static_cast<std::size_t>(offset);
  }

  std::size_t size = 2;
  switch (encoding) {
  case Encoding::euc_jp:
  case Encoding::euc_jis_2004:
    size = first == 0x8f ? 3 : 2;
    break;
  case Encoding::euc_tw:
    size = first == 0x8e ? 4 : 2;
    break;
  case Encoding::mule_internal:
    if (in_range(first, 0x81, 0x8d))
      size = 2;
    else if (in_range(first, 0x90, 0x9b))
      size = 3;
    else if (first == 0x9c || first == 0x9d)
      size = 4;
    else
      size = 1;
    break;
  case Encoding::sjis:
  case Encoding::shift_jis_2004:
    if (in_range(first, 0xa1, 0xdf))
      return std::size_t{1};
    break;
  case Encoding::gb18030:
    if (text.size() < 2)
      return invalid_character();
    size = in_range(static_cast<u8>(text[1]), 0x30, 0x39) ? 4 : 2;
    break;
  case Encoding::johab:
    size = first == 0x8f ? 3 : 2;
    break;
  default:
    break;
  }

  if (text.size() < size)
    return invalid_character();
  auto second = size > 1 ? static_cast<u8>(text[1]) : u8{0};
  bool valid = false;
  switch (encoding) {
  case Encoding::euc_jp:
  case Encoding::euc_jis_2004:
    if (first == 0x8e)
      valid = in_range(second, 0xa1, 0xdf);
    else if (first == 0x8f)
      valid = in_range(second, 0xa1, 0xfe) && in_range(static_cast<u8>(text[2]), 0xa1, 0xfe);
    else
      valid = in_range(first, 0xa1, 0xfe) && in_range(second, 0xa1, 0xfe);
    break;
  case Encoding::euc_cn:
  case Encoding::euc_kr:
    valid = in_range(first, 0xa1, 0xfe) && in_range(second, 0xa1, 0xfe);
    break;
  case Encoding::euc_tw:
    if (first == 0x8e) {
      valid = in_range(second, 0xa1, 0xa7) && in_range(static_cast<u8>(text[2]), 0xa1, 0xfe) &&
        in_range(static_cast<u8>(text[3]), 0xa1, 0xfe);
    } else
      valid = first != 0x8f && in_range(second, 0xa1, 0xfe);
    break;
  case Encoding::mule_internal:
    valid = std::all_of(text.begin() + 1, text.begin() + size, [](char byte) {
      return static_cast<u8>(byte) >= 0x80;
    });
    break;
  case Encoding::sjis:
  case Encoding::shift_jis_2004:
    valid = (in_range(first, 0x81, 0x9f) || in_range(first, 0xe0, 0xfc)) &&
      (in_range(second, 0x40, 0x7e) || in_range(second, 0x80, 0xfc));
    break;
  case Encoding::big5:
  case Encoding::gbk:
  case Encoding::uhc:
    // Match PostgreSQL's framing rules; quoting additionally rejects syntax bytes in a continuation.
    valid = second != 0 && !(first == 0x8d && second == 0x20);
    break;
  case Encoding::gb18030:
    valid = in_range(first, 0x81, 0xfe);
    if (size == 4)
      valid = valid && in_range(static_cast<u8>(text[2]), 0x81, 0xfe) && in_range(static_cast<u8>(text[3]), 0x30, 0x39);
    else
      valid = valid && (in_range(second, 0x40, 0x7e) || in_range(second, 0x80, 0xfe));
    break;
  case Encoding::johab:
    valid = std::all_of(text.begin() + 1, text.begin() + size, [](char byte) {
      return in_range(static_cast<u8>(byte), 0xa1, 0xfe);
    });
    break;
  default:
    break;
  }
  return valid ? Result<std::size_t>{size} : invalid_character();
}

Result<std::string> quote(std::string_view text, Encoding encoding, bool identifier, std::size_t limit)
{
  if (auto info = encoding_info(encoding); !info)
    return std::unexpected(info.error());

  char delimiter = identifier ? '"' : '\'';
  std::size_t size = identifier ? 2 : 3;
  if (size > limit || text.size() > limit - size)
    return std::unexpected(make_error_code(Error::resource_limit));
  size += text.size();

  for (std::size_t offset = 0; offset < text.size();) {
    auto length = encoded_character(text.substr(offset), encoding);
    if (!length)
      return std::unexpected(length.error());
    if (*length == 1) {
      char byte = text[offset];
      bool escaped = byte == delimiter || (!identifier && byte == '\\');
      if (escaped) {
        if (size == limit)
          return std::unexpected(make_error_code(Error::resource_limit));
        ++size;
      }
    } else {
      auto continuation = text.substr(offset + 1, *length - 1);
      bool syntax = std::any_of(continuation.begin(), continuation.end(), [](char byte) {
        return byte == '\'' || byte == '"';
      });
      if (syntax)
        return std::unexpected(std::make_error_code(std::errc::illegal_byte_sequence));
    }
    offset += *length;
  }

  std::string output;
  output.reserve(size);
  if (!identifier)
    output.push_back('E');
  output.push_back(delimiter);
  for (std::size_t offset = 0; offset < text.size();) {
    auto length = *encoded_character(text.substr(offset), encoding);
    char byte = text[offset];
    if (length == 1 && (byte == delimiter || (!identifier && byte == '\\')))
      output.push_back(byte);
    output.append(text.substr(offset, length));
    offset += length;
  }
  output.push_back(delimiter);
  return output;
}

int hex_digit(char byte) noexcept
{
  if (byte >= '0' && byte <= '9')
    return byte - '0';
  if (byte >= 'a' && byte <= 'f')
    return byte - 'a' + 10;
  if (byte >= 'A' && byte <= 'F')
    return byte - 'A' + 10;

  return -1;
}

bool hex_space(char byte) noexcept
{
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' || byte == '\f';
}

} // namespace

Result<Encoding> parse_encoding(std::string_view name) noexcept
{
  if (name.empty() || name.size() > 63)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  std::array<char, 64> normalized{};
  std::size_t size = 0;
  for (char byte : name) {
    auto value = static_cast<u8>(byte);
    if (!value || value >= 0x80)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (byte >= 'A' && byte <= 'Z')
      byte = static_cast<char>(byte - 'A' + 'a');
    if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9'))
      normalized[size++] = byte;
  }
  auto key = std::string_view{normalized.data(), size};
  if (key.size() > 7 && key.starts_with("windows")) {
    std::move(normalized.begin() + 7, normalized.begin() + size, normalized.begin() + 3);
    size -= 4;
    key = {normalized.data(), size};
  }
  for (std::size_t index = 0; index < encodings.size(); ++index) {
    if (encoding_name_equal(key, encodings[index].name))
      return static_cast<Encoding>(index);
  }
  for (const auto &alias : aliases) {
    if (key == alias.name)
      return alias.encoding;
  }
  return std::unexpected(std::make_error_code(std::errc::invalid_argument));
}

Result<EncodingInfo> encoding_info(Encoding encoding) noexcept
{
  auto index = static_cast<std::size_t>(encoding);
  if (index >= encodings.size())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return encodings[index];
}

Result<std::size_t> character_size(std::string_view text, Encoding encoding) noexcept
{
  if (auto info = encoding_info(encoding); !info)
    return std::unexpected(info.error());
  return encoded_character(text, encoding);
}

Result<void> validate_text(std::string_view text, Encoding encoding) noexcept
{
  if (auto info = encoding_info(encoding); !info)
    return std::unexpected(info.error());
  for (std::size_t offset = 0; offset < text.size();) {
    auto size = encoded_character(text.substr(offset), encoding);
    if (!size)
      return std::unexpected(size.error());
    offset += *size;
  }
  return {};
}

Result<i32> character_width(std::string_view text, Encoding encoding) noexcept
{
  auto size = character_size(text, encoding);
  if (!size)
    return std::unexpected(size.error());
  if (!*size)
    return i32{0};

  auto first = static_cast<u8>(text.front());
  if (encoding == Encoding::mule_internal)
    return i32{in_range(first, 0x90, 0x99) || first == 0x9c || first == 0x9d ? 2 : 1};
  if (first < 0x20 || first == 0x7f)
    return i32{-1};
  if (first < 0x80)
    return i32{1};

  if (encoding == Encoding::utf8) {
    int32_t offset = 0;
    UChar32 value = 0;
    U8_NEXT(text.data(), offset, static_cast<int32_t>(*size), value);
    if (u_charType(value) == U_CONTROL_CHAR)
      return i32{-1};
    auto category = u_charType(value);
    if (category == U_NON_SPACING_MARK || category == U_ENCLOSING_MARK || category == U_FORMAT_CHAR ||
      (value >= 0x1160 && value <= 0x11ff))
      return i32{0};
    auto width = u_getIntPropertyValue(value, UCHAR_EAST_ASIAN_WIDTH);
    return i32{width == U_EA_WIDE || width == U_EA_FULLWIDTH ? 2 : 1};
  }
  if ((encoding == Encoding::euc_jp || encoding == Encoding::euc_jis_2004) && first == 0x8e)
    return i32{1};
  return i32{*size == 1 ? 1 : 2};
}

Result<std::string> escape_literal(std::string_view text, Encoding encoding, std::size_t limit)
{
  return quote(text, encoding, false, limit);
}

Result<std::string> escape_identifier(std::string_view text, Encoding encoding, std::size_t limit)
{
  return quote(text, encoding, true, limit);
}

Result<std::string> encode_bytea(std::span<const std::byte> bytes, std::size_t limit)
{
  if (limit < 2 || bytes.size() > (limit - 2) / 2)
    return std::unexpected(make_error_code(Error::resource_limit));

  constexpr std::string_view digits = "0123456789abcdef";
  std::string output = "\\x";
  output.reserve(bytes.size() * 2 + 2);
  for (auto byte : bytes) {
    auto value = std::to_integer<unsigned char>(byte);
    output.push_back(digits[value >> 4]);
    output.push_back(digits[value & 15]);
  }

  return output;
}

Result<std::vector<std::byte>> decode_bytea(std::string_view text, std::size_t limit)
{
  std::vector<std::byte> output;
  bool hex = text.starts_with("\\x");
  std::size_t offset = hex ? 2 : 0;
  while (offset < text.size()) {
    unsigned value = 0;
    if (hex) {
      if (hex_space(text[offset])) {
        ++offset;
        continue;
      }

      if (text.size() - offset < 2)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      auto high = hex_digit(text[offset]);
      auto low = hex_digit(text[offset + 1]);
      if (high < 0 || low < 0)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

      value = static_cast<unsigned>(high * 16 + low);
      offset += 2;
    } else if (text[offset] == '\\') {
      ++offset;
      if (offset < text.size() && text[offset] == '\\') {
        value = '\\';
        ++offset;
      } else {
        if (text.size() - offset < 3 || text[offset] < '0' || text[offset] > '3' || text[offset + 1] < '0' ||
          text[offset + 1] > '7' || text[offset + 2] < '0' || text[offset + 2] > '7')
          return std::unexpected(std::make_error_code(std::errc::invalid_argument));

        value = static_cast<unsigned>(
          (text[offset] - '0') * 64 + (text[offset + 1] - '0') * 8 + text[offset + 2] - '0');
        offset += 3;
      }
    } else {
      value = static_cast<unsigned char>(text[offset++]);
    }

    if (output.size() == limit)
      return std::unexpected(make_error_code(Error::resource_limit));
    output.push_back(static_cast<std::byte>(value));
  }

  return output;
}

} // namespace weave::pg
