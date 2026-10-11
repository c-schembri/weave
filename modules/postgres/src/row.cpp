#include <weave/postgres/connection.hpp>
#include "row_schema.hpp"
#include <weave/database/selection.hpp>

namespace weave::pg {

namespace {

class DecodeCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.postgres.decode";
  }

  std::string message(int code) const override
  {
    switch (static_cast<DecodeError>(code)) {
    case DecodeError::null_value:
      return "SQL NULL requires an optional target";
    case DecodeError::invalid_value:
      return "Invalid PostgreSQL primitive value";
    case DecodeError::out_of_range:
      return "PostgreSQL value or column index is out of range";
    case DecodeError::unsupported_type:
      return "Unsupported PostgreSQL primitive conversion";
    case DecodeError::missing_column:
      return "PostgreSQL column was not found";
    case DecodeError::ambiguous_column:
      return "PostgreSQL column name is ambiguous";
    case DecodeError::schema_mismatch:
      return "PostgreSQL column handle or decoder belongs to another schema";
    }
    return "Unknown PostgreSQL decoding error";
  }
};

Result<std::size_t> find_column(const detail::RowSchema &schema, std::string_view name)
{
  auto selected = db::find_column(schema.columns, name);
  if (!selected) {
    const auto error = selected.error() == db::ColumnError::ambiguous ? DecodeError::ambiguous_column
                                                                      : DecodeError::missing_column;
    return std::unexpected(make_error_code(error));
  }
  return *selected;
}

u64 network_number(std::string_view bytes) noexcept
{
  u64 number = 0;
  for (unsigned char byte : bytes)
    number = (number << 8) | byte;
  return number;
}

} // namespace

std::error_code make_error_code(DecodeError error) noexcept
{
  static DecodeCategory category;
  return {static_cast<int>(error), category};
}

Result<bool> Value::boolean_value() const noexcept
{
  auto text = bytes();
  if (format == Format::text) {
    if (text == "t")
      return true;
    if (text == "f")
      return false;
  } else if (type == 16) {
    if (text.size() == 1 && text[0] == '\1')
      return true;
    if (text.size() == 1 && text[0] == '\0')
      return false;
  } else {
    return std::unexpected(make_error_code(DecodeError::unsupported_type));
  }
  return std::unexpected(make_error_code(DecodeError::invalid_value));
}

Result<std::string_view> Value::text_value() const noexcept
{
  if (format == Format::text || type == 18 || type == 19 || type == 25 || type == 1042 || type == 1043)
    return bytes();
  return std::unexpected(make_error_code(DecodeError::unsupported_type));
}

Result<i64> Value::binary_number() const noexcept
{
  auto text = bytes();
  if (type == 21 && text.size() == 2)
    return std::bit_cast<i16>(static_cast<u16>(network_number(text)));
  if (type == 23 && text.size() == 4)
    return std::bit_cast<i32>(static_cast<u32>(network_number(text)));
  if (type == 20 && text.size() == 8)
    return std::bit_cast<i64>(network_number(text));
  if (type == 26 && text.size() == 4)
    return static_cast<i64>(network_number(text));
  if (type == 21 || type == 23 || type == 20 || type == 26)
    return std::unexpected(make_error_code(DecodeError::invalid_value));
  return std::unexpected(make_error_code(DecodeError::unsupported_type));
}

Result<double> Value::binary_floating() const noexcept
{
  auto text = bytes();
  if (type == 700 && text.size() == 4)
    return static_cast<double>(std::bit_cast<float>(static_cast<u32>(network_number(text))));
  if (type == 701 && text.size() == 8)
    return std::bit_cast<double>(network_number(text));
  if (type == 700 || type == 701)
    return std::unexpected(make_error_code(DecodeError::invalid_value));
  auto number = binary_number();
  if (!number)
    return std::unexpected(number.error());
  return static_cast<double>(*number);
}

bool Field::is_null() const noexcept
{
  weave::detail::require(value_ != nullptr && !error_);
  return value_->is_null();
}

std::string_view Field::bytes() const noexcept
{
  weave::detail::require(value_ != nullptr && !error_);
  return value_->bytes();
}

Field Row::operator[](std::size_t index) const & noexcept
{
  if (index >= size())
    return Field{make_error_code(DecodeError::out_of_range)};
  return Field{Base::operator[](index)};
}

Field Row::operator[](std::string_view name) const & noexcept
{
  if (!schema_)
    return Field{make_error_code(DecodeError::missing_column)};
  auto index = find_column(*schema_, name);
  if (!index)
    return Field{index.error()};
  return (*this)[*index];
}

Field Row::operator[](const ColumnHandle &column) const & noexcept
{
  if (!schema_ || schema_ != column.schema_)
    return Field{make_error_code(DecodeError::schema_mismatch)};
  return (*this)[column.index_];
}

RowReader Row::reader() const & noexcept
{
  return RowReader{*this};
}

Field RowReader::next() noexcept
{
  if (remaining() == 0)
    return Field{make_error_code(DecodeError::out_of_range)};
  return (*row_)[position_++];
}

std::size_t RowReader::remaining() const noexcept
{
  return position_ < row_->size() ? row_->size() - position_ : 0;
}

Result<void> RowReader::skip(std::size_t count) noexcept
{
  if (count > remaining())
    return std::unexpected(make_error_code(DecodeError::out_of_range));
  position_ += count;
  return {};
}

Result<ColumnHandle> ResultSet::column(std::string_view name) const
{
  if (!row_schema_)
    return std::unexpected(make_error_code(DecodeError::missing_column));
  auto index = find_column(*row_schema_, name);
  if (!index)
    return std::unexpected(index.error());
  return ColumnHandle{row_schema_, *index};
}

} // namespace weave::pg
