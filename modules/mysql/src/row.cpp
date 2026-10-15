#include <weave/mysql/row.hpp>
#include <weave/database/selection.hpp>

namespace weave::mysql {

namespace {

class ClientCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.mysql";
  }

  std::string message(int value) const override
  {
    switch (static_cast<ClientError>(value)) {
    case ClientError::invalid_options:
      return "Invalid MySQL options";
    case ClientError::protocol:
      return "Invalid MySQL protocol message";
    case ClientError::resource_limit:
      return "MySQL resource limit exceeded";
    case ClientError::unsupported_authentication:
      return "Unsupported MySQL authentication";
    case ClientError::tls_required:
      return "Verified MySQL TLS required";
    case ClientError::closed:
      return "MySQL session is closed";
    case ClientError::busy:
      return "MySQL session is busy";
    case ClientError::null_value:
      return "MySQL value is NULL";
    case ClientError::invalid_value:
      return "Invalid MySQL value";
    case ClientError::out_of_range:
      return "MySQL value is out of range";
    case ClientError::missing_column:
      return "MySQL column is missing";
    case ClientError::ambiguous_column:
      return "MySQL column is ambiguous";
    case ClientError::multiple_results:
      return "MySQL command returned multiple results; use query() or stream()";
    case ClientError::unsupported:
      return "Unsupported MySQL operation or type";
    }
    return "Unknown MySQL client error";
  }
};

class ServerCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "mysql.server";
  }

  std::string message(int value) const override
  {
    return "MySQL server error " + std::to_string(value);
  }
};

} // namespace

std::error_code make_error_code(ClientError error) noexcept
{
  static ClientCategory category;
  return {static_cast<int>(error), category};
}

std::error_code server_error(u16 number) noexcept
{
  static ServerCategory category;
  return {number, category};
}

Field Row::operator[](std::string_view name) const & noexcept
{
  if (!columns_)
    return Field{ClientError::missing_column};

  auto index = db::find_column(*columns_, name);
  if (!index) {
    if (index.error() == db::ColumnError::ambiguous)
      return Field{ClientError::ambiguous_column};
    return Field{ClientError::missing_column};
  }
  return (*this)[*index];
}

Result<DateTime> Field::datetime() const
{
  auto data = bytes();
  if (!data)
    return std::unexpected(data.error());
  const auto type_ = metadata().type;
  if (type_ != 7 && type_ != 10 && type_ != 12)
    return std::unexpected(make_error_code(ClientError::invalid_value));
  DateTime result;
  if (format() == Format::binary) {
    if (data->empty())
      return std::unexpected(make_error_code(ClientError::invalid_value));
    const auto size = static_cast<unsigned char>((*data)[0]);
    if (data->size() != std::size_t{size} + 1 ||
      !(size == 0 || size == 4 || (type_ != 10 && (size == 7 || size == 11))))
      return std::unexpected(make_error_code(ClientError::invalid_value));
    auto integer = [&](std::size_t offset, std::size_t count) {
      u32 value = 0;
      for (std::size_t index = 0; index < count; ++index)
        value |= u32{static_cast<unsigned char>((*data)[offset + index])} << (8 * index);
      return value;
    };
    if (size >= 4)
      result.date = {static_cast<u16>(integer(1, 2)), static_cast<u8>(integer(3, 1)), static_cast<u8>(integer(4, 1))};
    if (size >= 7) {
      result.hour = static_cast<u8>(integer(5, 1));
      result.minute = static_cast<u8>(integer(6, 1));
      result.second = static_cast<u8>(integer(7, 1));
    }
    if (size == 11)
      result.microsecond = integer(8, 4);
  } else {
    const bool date = type_ == 10;
    if ((date && data->size() != 10) || (!date && data->size() != 19 && (data->size() < 21 || data->size() > 26)))
      return std::unexpected(make_error_code(ClientError::invalid_value));
    bool valid = true;
    auto integer = [&](std::size_t offset, std::size_t count) {
      u32 value = 0;
      auto parsed = std::from_chars(data->data() + offset, data->data() + offset + count, value);
      if (parsed.ec != std::errc{} || parsed.ptr != data->data() + offset + count)
        valid = false;
      return value;
    };
    result.date = {static_cast<u16>(integer(0, 4)), static_cast<u8>(integer(5, 2)), static_cast<u8>(integer(8, 2))};
    valid = valid && (*data)[4] == '-' && (*data)[7] == '-';
    if (!date) {
      result.hour = static_cast<u8>(integer(11, 2));
      result.minute = static_cast<u8>(integer(14, 2));
      result.second = static_cast<u8>(integer(17, 2));
      valid = valid && (*data)[10] == ' ' && (*data)[13] == ':' && (*data)[16] == ':';
      if (data->size() > 19) {
        valid = valid && (*data)[19] == '.';
        const auto digits = data->size() - 20;
        result.microsecond = integer(20, digits);
        for (std::size_t count = digits; count < 6; ++count)
          result.microsecond *= 10;
      }
    }
    if (!valid)
      return std::unexpected(make_error_code(ClientError::invalid_value));
  }
  if (result.date.year > 9999 || result.date.month > 12 || result.date.day > 31 || result.hour > 23 ||
    result.minute > 59 || result.second > 59 || result.microsecond > 999999)
    return std::unexpected(make_error_code(ClientError::invalid_value));
  return result;
}

Result<Date> Field::date() const
{
  auto value = datetime();
  if (!value)
    return std::unexpected(value.error());
  return value->date;
}

Result<Time> Field::time() const
{
  auto data = bytes();
  if (!data)
    return std::unexpected(data.error());
  const auto type_ = metadata().type;
  if (type_ != 11)
    return std::unexpected(make_error_code(ClientError::invalid_value));
  Time result;
  if (format() == Format::binary) {
    if (data->empty())
      return std::unexpected(make_error_code(ClientError::invalid_value));
    const auto size = static_cast<unsigned char>((*data)[0]);
    if (data->size() != std::size_t{size} + 1 || !(size == 0 || size == 8 || size == 12))
      return std::unexpected(make_error_code(ClientError::invalid_value));
    auto integer = [&](std::size_t offset, std::size_t count) {
      u32 value = 0;
      for (std::size_t index = 0; index < count; ++index)
        value |= u32{static_cast<unsigned char>((*data)[offset + index])} << (8 * index);
      return value;
    };
    if (size) {
      if (integer(1, 1) > 1)
        return std::unexpected(make_error_code(ClientError::invalid_value));
      result.negative = integer(1, 1) != 0;
      result.days = integer(2, 4);
      result.hour = static_cast<u8>(integer(6, 1));
      result.minute = static_cast<u8>(integer(7, 1));
      result.second = static_cast<u8>(integer(8, 1));
    }
    if (size == 12)
      result.microsecond = integer(9, 4);
  } else {
    auto text = *data;
    if (text.starts_with('-')) {
      result.negative = true;
      text.remove_prefix(1);
    }
    const auto separator = text.find(':');
    if (separator == std::string_view::npos || separator < 2 || separator > 3 ||
      (text.size() != separator + 6 && (text.size() < separator + 8 || text.size() > separator + 13)) ||
      text[separator + 3] != ':')
      return std::unexpected(make_error_code(ClientError::invalid_value));
    bool valid = true;
    auto integer = [&](std::size_t offset, std::size_t count) {
      u32 value = 0;
      auto parsed = std::from_chars(text.data() + offset, text.data() + offset + count, value);
      valid = valid && parsed.ec == std::errc{} && parsed.ptr == text.data() + offset + count;
      return value;
    };
    const auto hours = integer(0, separator);
    result.days = hours / 24;
    result.hour = static_cast<u8>(hours % 24);
    result.minute = static_cast<u8>(integer(separator + 1, 2));
    result.second = static_cast<u8>(integer(separator + 4, 2));
    if (text.size() > separator + 6) {
      valid = valid && text[separator + 6] == '.';
      const auto digits = text.size() - separator - 7;
      result.microsecond = integer(separator + 7, digits);
      for (std::size_t count = digits; count < 6; ++count)
        result.microsecond *= 10;
    }
    if (!valid)
      return std::unexpected(make_error_code(ClientError::invalid_value));
  }
  if (result.days > 34 || result.days * 24 + result.hour > 838 || result.hour > 23 || result.minute > 59 ||
    result.second > 59 || result.microsecond > 999999)
    return std::unexpected(make_error_code(ClientError::invalid_value));
  return result;
}

} // namespace weave::mysql
