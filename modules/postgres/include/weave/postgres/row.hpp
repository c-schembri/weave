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

/// PostgreSQL wire encoding, not the desired C++ conversion type.
enum class Format : u16 {
  text,
  binary
};

/// Checked field/record conversion failures, including NULL, range and schema errors.
enum class DecodeError {
  null_value = 1,
  invalid_value,
  out_of_range,
  unsupported_type,
  missing_column,
  ambiguous_column,
  schema_mismatch
};

/// Convert a decode failure to the standard error-code channel.
std::error_code make_error_code(DecodeError error) noexcept;

/// Retained PostgreSQL field metadata: name, source relation/attribute, type OID and wire format.
struct Column {
  detail::ResultText name;
  u32 table = 0;
  i16 attribute = 0;
  u32 type = 0;
  i16 type_size = 0;
  i32 modifier = 0;
  Format format = Format::text;
};

/// Retained wire value. nullopt is SQL NULL; an engaged empty value is not NULL.
struct Value {
  std::optional<detail::ResultText> data;
  Format format = Format::text;
  u32 type = 0;

  /// Whether the server supplied SQL NULL.
  bool is_null() const noexcept;
  /// Borrow wire bytes, not decoded bytea; NULL and empty both yield an empty view.
  /// The view expires when its retained backing storage is released or replaced.
  std::string_view bytes() const noexcept;

  /// Decode bool, integer, float/double, string/string_view, or optional of a supported primitive.
  /// Uses text/binary metadata and checked ranges; NULL needs optional<T>. string owns a copy,
  /// string_view borrows backing bytes. Binary conversion supports known compatible PostgreSQL OIDs.
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

  /// Low-level text-only integer parse; unlike as<T>(), does not select a binary decoder.
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

  /// Interpret exactly sizeof(T) network-order binary bytes without checking a PostgreSQL type OID.
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

/// Borrowed checked selector into a Row/Value; conversions return stored selection errors.
/// Must not outlive or be used after mutation of the selected row/value.
class Field {
  const Value *value_ = nullptr;
  std::error_code error_;

public:
  const Format format;

  /// Borrow a retained Value without copying its payload.
  explicit Field(const Value &value) noexcept : value_(&value), format(value.format)
  {
  }

  /// Construct a failed selector; checked conversions return this error.
  explicit Field(std::error_code error) noexcept : error_(error), format(Format::text)
  {
  }

  /// Decode a supported primitive/optional with checked selection, NULL and range handling.
  /// string_view results borrow the selected value; string results own a copy.
  template <class T>
    requires detail::primitive_value<T>
  Result<T> as() const
  {
    if (!value_)
      return std::unexpected(error_);
    return value_->as<T>();
  }

  /// Checked integer conversion, defaulting to i64; accepts supported text/binary values.
  template <class T = i64>
    requires(std::is_integral_v<T> && !std::same_as<T, bool> && sizeof(T) <= 8)
  Result<T> integer() const
  {
    return as<T>();
  }

  /// Checked floating-point conversion, defaulting to double.
  template <class T = double>
    requires(std::same_as<T, float> || std::same_as<T, double>)
  Result<T> floating() const
  {
    return as<T>();
  }

  /// Decode a boolean or return a NULL/type/value error.
  Result<bool> boolean() const
  {
    return as<bool>();
  }

  /// Decode text as a borrowed view; keep backing value storage alive and unchanged.
  Result<std::string_view> text() const
  {
    return as<std::string_view>();
  }

  /// Decode text into an owning string copy.
  Result<std::string> string() const
  {
    return as<std::string>();
  }

  /// Map SQL NULL to nullopt; otherwise use checked as<T>() conversion.
  template <class T>
    requires detail::primitive_value<T>
  Result<std::optional<T>> optional() const
  {
    return as<std::optional<T>>();
  }

  /// Low-level fixed-width network-order decode; does not validate a PostgreSQL type OID.
  template <class T>
  Result<T> binary_integer() const
  {
    if (!value_)
      return std::unexpected(error_);
    return value_->binary_integer<T>();
  }

  /// Inspect NULL on a valid selector; invalid selectors violate the raw-inspection contract.
  bool is_null() const noexcept;
  /// Borrow raw wire bytes from a valid selector; prefer checked conversions for invalid selectors.
  std::string_view bytes() const noexcept;
};

/// Cached index retaining schema identity, not row payload. Use only with rows from the same schema.
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
  /// Zero-based column position; bypassing schema checks with this index is the caller's responsibility.
  std::size_t index() const noexcept
  {
    return index_;
  }
};

class RowReader;
template <class T>
class RowDecoder;

/// Retained row values with checked index/name/schema-bound selection and struct decoding.
/// Field selectors and readers borrow the Row; keep it alive and unchanged while they are used.
class Row : public detail::ResultList<Value> {
  using Base = detail::ResultList<Value>;
  std::shared_ptr<const detail::RowSchema> schema_;
  friend struct detail::RowAccess;
  template <class T>
  friend class RowDecoder;

public:
  using Base::Base;

  /// Select a zero-based column; an out-of-range selector reports an error on conversion.
  Field operator[](std::size_t index) const & noexcept;
  /// Select an exact case-sensitive name; missing or duplicate names fail checked conversion.
  Field operator[](std::string_view name) const & noexcept;
  /// Select using a cached handle; reject handles from another schema.
  Field operator[](const ColumnHandle &column) const & noexcept;
  Field operator[](std::size_t) const && = delete;
  Field operator[](std::string_view) const && = delete;
  Field operator[](const ColumnHandle &) const && = delete;

  /// Select by index, name or ColumnHandle and decode a primitive in one checked synchronous call.
  template <class T, class Selector>
    requires detail::primitive_value<T>
  Result<T> get(const Selector &selector) const &
  {
    return (*this)[selector].template as<T>();
  }

  template <class T, class Selector>
  Result<T> get(const Selector &) const && = delete;

  /// Map matching column/member names to T without registration; RowTraits<T> optionally supplies aliases.
  /// Owning members are copied; string_view members borrow row data, which must remain alive and unchanged.
  /// Returns a synchronous Result<T>, not an awaitable. Temporary rows are rejected.
  template <class T>
  Result<T> as() const &;

  template <class T>
  Result<T> as() const && = delete;

  /// Create a borrowed sequential column reader at position zero; rejects temporary rows.
  RowReader reader() const & noexcept;
  RowReader reader() const && = delete;

  /// Borrow mutable low-level values; edits can invalidate selectors/views and metadata assumptions.
  std::span<Value> values() noexcept
  {
    return {data(), size()};
  }

  /// Borrow retained wire values without a copy; span expires on row mutation/destruction.
  std::span<const Value> values() const noexcept
  {
    return {data(), size()};
  }
};

/// Sequential borrowed column cursor; it does not iterate rows or own the Row.
class RowReader {
  const Row *row_;
  std::size_t position_ = 0;

public:
  /// Start at column zero; the Row must outlive the reader and remain unchanged.
  explicit RowReader(const Row &row) noexcept : row_(&row)
  {
  }

  RowReader(Row &&) = delete;
  RowReader(const Row &&) = delete;

  /// Select and advance one column; exhaustion produces a failed Field, not unchecked access.
  Field next() noexcept;

  /// Select/advance once and decode T; conversion failure does not rewind the cursor.
  template <class T>
    requires detail::primitive_value<T>
  Result<T> next()
  {
    return next().template as<T>();
  }

  /// Number of unvisited columns, not unread rows.
  std::size_t remaining() const noexcept;

  /// Rewind to column zero without changing the Row.
  void reset() noexcept
  {
    position_ = 0;
  }

  /// Advance by count, or return a bounds error without stepping beyond the row.
  Result<void> skip(std::size_t count) noexcept;
};

} // namespace weave::pg

template <>
struct std::is_error_code_enum<weave::pg::DecodeError> : std::true_type {};
