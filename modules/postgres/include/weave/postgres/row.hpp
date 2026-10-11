#pragma once

#include <weave/postgres/detail/result_storage.hpp>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace weave::pg {

namespace detail {

struct RowSchema;
struct RowAccess;

template <class T>
struct OptionalValue : std::false_type {};

template <class T>
struct OptionalValue<std::optional<T>> : std::true_type {
  using Type = T;
};

template <class T>
inline constexpr bool primitive_value = [] {
  if constexpr (OptionalValue<T>::value)
    return primitive_value<typename OptionalValue<T>::Type>;
  else
    return (std::is_integral_v<T> && sizeof(T) <= 8) || std::same_as<T, float> || std::same_as<T, double> ||
      std::same_as<T, std::string> || std::same_as<T, std::string_view>;
}();

} // namespace detail

enum class Format : u16 {
  text,
  binary
};

enum class DecodeError {
  null_value = 1,
  invalid_value,
  out_of_range,
  unsupported_type,
  missing_column,
  ambiguous_column,
  schema_mismatch
};

std::error_code make_error_code(DecodeError error) noexcept;

struct Column {
  detail::ResultText name;
  u32 table = 0;
  i16 attribute = 0;
  u32 type = 0;
  i16 type_size = 0;
  i32 modifier = 0;
  Format format = Format::text;
};

struct Value {
  std::optional<detail::ResultText> data;
  Format format = Format::text;
  u32 type = 0;

  bool is_null() const noexcept;
  std::string_view bytes() const noexcept;

  template <class T>
    requires detail::primitive_value<T>
  Result<T> as() const
  {
    if constexpr (detail::OptionalValue<T>::value) {
      if (!data)
        return T{};
      auto decoded = as<typename detail::OptionalValue<T>::Type>();
      if (!decoded)
        return std::unexpected(decoded.error());
      return T{std::move(*decoded)};
    } else {
      if (!data)
        return std::unexpected(make_error_code(DecodeError::null_value));
      if (format != Format::text && format != Format::binary)
        return std::unexpected(make_error_code(DecodeError::unsupported_type));

      if constexpr (std::same_as<T, bool>) {
        return boolean_value();
      } else if constexpr (std::same_as<T, std::string_view> || std::same_as<T, std::string>) {
        auto decoded = text_value();
        if (!decoded)
          return std::unexpected(decoded.error());
        return T{*decoded};
      } else if constexpr (std::is_integral_v<T>) {
        using Number = std::conditional_t<std::is_signed_v<T>, i64, u64>;
        Number number = 0;
        if (format == Format::binary) {
          auto decoded = binary_number();
          if (!decoded)
            return std::unexpected(decoded.error());
          if constexpr (!std::is_signed_v<T>) {
            if (*decoded < 0)
              return std::unexpected(make_error_code(DecodeError::out_of_range));
          }
          number = static_cast<Number>(*decoded);
        } else {
          auto text = bytes();
          if (text.empty())
            return std::unexpected(make_error_code(DecodeError::invalid_value));
          auto decoded = std::from_chars(text.data(), text.data() + text.size(), number);
          if (decoded.ec == std::errc::result_out_of_range)
            return std::unexpected(make_error_code(DecodeError::out_of_range));
          if (decoded.ec != std::errc{} || decoded.ptr != text.data() + text.size())
            return std::unexpected(make_error_code(DecodeError::invalid_value));
        }
        if (number < static_cast<Number>(std::numeric_limits<T>::min()) ||
          number > static_cast<Number>(std::numeric_limits<T>::max()))
          return std::unexpected(make_error_code(DecodeError::out_of_range));
        return static_cast<T>(number);
      } else {
        T number = 0;
        if (format == Format::binary) {
          auto decoded = binary_floating();
          if (!decoded)
            return std::unexpected(decoded.error());
          if (std::isfinite(*decoded) && std::abs(*decoded) > std::numeric_limits<T>::max())
            return std::unexpected(make_error_code(DecodeError::out_of_range));
          number = static_cast<T>(*decoded);
          if (*decoded != 0 && number == 0)
            return std::unexpected(make_error_code(DecodeError::out_of_range));
        } else {
          auto text = bytes();
          if (text == "Infinity")
            return std::numeric_limits<T>::infinity();
          if (text == "-Infinity")
            return -std::numeric_limits<T>::infinity();
          if (text == "NaN")
            return std::numeric_limits<T>::quiet_NaN();
          if (text.empty())
            return std::unexpected(make_error_code(DecodeError::invalid_value));
          auto decoded = std::from_chars(text.data(), text.data() + text.size(), number);
          if (decoded.ec == std::errc::result_out_of_range)
            return std::unexpected(make_error_code(DecodeError::out_of_range));
          if (decoded.ec != std::errc{} || decoded.ptr != text.data() + text.size())
            return std::unexpected(make_error_code(DecodeError::invalid_value));
        }
        return number;
      }
    }
  }

  // Retain the existing explicit wire-format decoders for low-level callers.
  template <class T>
    requires(std::is_integral_v<T> && !std::same_as<T, bool>)
  Result<T> integer() const noexcept
  {
    if (format != Format::text)
      return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    if (!data)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    T value{};
    auto parsed = std::from_chars(data->data(), data->data() + data->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != data->data() + data->size())
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    return value;
  }

  template <class T>
    requires(std::is_integral_v<T> && !std::same_as<T, bool> && sizeof(T) <= 8)
  Result<T> binary_integer() const noexcept
  {
    if (format != Format::binary || !data || data->size() != sizeof(T))
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    std::make_unsigned_t<T> value = 0;
    for (auto byte : *data)
      value = static_cast<std::make_unsigned_t<T>>((value << 8) | static_cast<unsigned char>(byte));
    return std::bit_cast<T>(value);
  }

private:
  Result<bool> boolean_value() const noexcept;
  Result<std::string_view> text_value() const noexcept;
  Result<i64> binary_number() const noexcept;
  Result<double> binary_floating() const noexcept;
};

class Field {
  const Value *value_ = nullptr;
  std::error_code error_;

public:
  const Format format;

  explicit Field(const Value &value) noexcept : value_(&value), format(value.format)
  {
  }

  explicit Field(std::error_code error) noexcept : error_(error), format(Format::text)
  {
  }

  template <class T>
    requires detail::primitive_value<T>
  Result<T> as() const
  {
    if (!value_)
      return std::unexpected(error_);
    return value_->as<T>();
  }

  template <class T = i64>
    requires(std::is_integral_v<T> && !std::same_as<T, bool> && sizeof(T) <= 8)
  Result<T> integer() const
  {
    return as<T>();
  }

  template <class T = double>
    requires(std::same_as<T, float> || std::same_as<T, double>)
  Result<T> floating() const
  {
    return as<T>();
  }

  Result<bool> boolean() const
  {
    return as<bool>();
  }

  Result<std::string_view> text() const
  {
    return as<std::string_view>();
  }

  Result<std::string> string() const
  {
    return as<std::string>();
  }

  template <class T>
    requires detail::primitive_value<T>
  Result<std::optional<T>> optional() const
  {
    return as<std::optional<T>>();
  }

  template <class T>
  Result<T> binary_integer() const
  {
    if (!value_)
      return std::unexpected(error_);
    return value_->binary_integer<T>();
  }

  // Raw inspection requires a valid selector; use checked conversions otherwise.
  bool is_null() const noexcept;
  std::string_view bytes() const noexcept;
};

class ColumnHandle {
  std::shared_ptr<const detail::RowSchema> schema_;
  std::size_t index_ = 0;
  friend class Row;
  friend struct ResultSet;

  ColumnHandle(std::shared_ptr<const detail::RowSchema> schema, std::size_t index) noexcept
      : schema_(std::move(schema)), index_(index)
  {
  }

public:
  std::size_t index() const noexcept
  {
    return index_;
  }
};

class RowReader;
template <class T>
class RowDecoder;

class Row : public detail::ResultList<Value> {
  using Base = detail::ResultList<Value>;
  std::shared_ptr<const detail::RowSchema> schema_;
  friend struct detail::RowAccess;
  template <class T>
  friend class RowDecoder;

public:
  using Base::Base;

  Field operator[](std::size_t index) const & noexcept;
  Field operator[](std::string_view name) const & noexcept;
  Field operator[](const ColumnHandle &column) const & noexcept;
  Field operator[](std::size_t) const && = delete;
  Field operator[](std::string_view) const && = delete;
  Field operator[](const ColumnHandle &) const && = delete;

  template <class T, class Selector>
    requires detail::primitive_value<T>
  Result<T> get(const Selector &selector) const &
  {
    return (*this)[selector].template as<T>();
  }

  template <class T, class Selector>
  Result<T> get(const Selector &) const && = delete;

  template <class T>
  Result<T> as() const &;

  template <class T>
  Result<T> as() const && = delete;

  RowReader reader() const & noexcept;
  RowReader reader() const && = delete;

  // Explicit low-level storage access for editing/retaining owning wire values.
  std::span<Value> values() noexcept
  {
    return {data(), size()};
  }

  std::span<const Value> values() const noexcept
  {
    return {data(), size()};
  }
};

class RowReader {
  const Row *row_;
  std::size_t position_ = 0;

public:
  explicit RowReader(const Row &row) noexcept : row_(&row)
  {
  }

  RowReader(Row &&) = delete;
  RowReader(const Row &&) = delete;

  Field next() noexcept;

  template <class T>
    requires detail::primitive_value<T>
  Result<T> next()
  {
    return next().template as<T>();
  }

  std::size_t remaining() const noexcept;

  void reset() noexcept
  {
    position_ = 0;
  }

  Result<void> skip(std::size_t count) noexcept;
};

} // namespace weave::pg

template <>
struct std::is_error_code_enum<weave::pg::DecodeError> : std::true_type {};
