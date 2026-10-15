#pragma once

#include <weave/mysql/error.hpp>
#include <weave/database/detail/result_storage.hpp>
#include <charconv>
#include <cmath>
#include <optional>
#include <memory>
#include <bit>
#include <limits>
#include <cstring>
#include <type_traits>
#include <weave/mysql/parameter.hpp>

namespace weave::mysql {

class Connection;
class RowStream;
class Row;

enum class Format {
  text,
  binary
};

/// MySQL metadata, including its native type/charset/flags. Names and metadata are retained.
struct Column {
  db::detail::ResultText schema;
  db::detail::ResultText table;
  db::detail::ResultText original_table;
  db::detail::ResultText name;
  db::detail::ResultText original_name;
  u16 character_set = 0;
  u32 length = 0;
  u8 type = 0;
  u16 flags = 0;
  u8 decimals = 0;
};

namespace detail {

template <class T>
struct Optional {
  static constexpr bool value = false;
};

template <class T>
struct Optional<std::optional<T>> {
  static constexpr bool value = true;
  using Type = T;
};

template <class T>
constexpr bool integer = std::integral<T> && sizeof(T) <= 8 && std::same_as<T, std::remove_cv_t<T>> &&
  !std::same_as<T, bool> && !std::same_as<T, char> && !std::same_as<T, wchar_t> && !std::same_as<T, char8_t> &&
  !std::same_as<T, char16_t> && !std::same_as<T, char32_t>;

template <class T>
constexpr bool primitive = std::same_as<T, bool> || integer<T> || std::same_as<T, float> || std::same_as<T, double> ||
  std::same_as<T, std::string> || std::same_as<T, std::string_view> || std::same_as<T, Date> ||
  std::same_as<T, DateTime> || std::same_as<T, Time> || std::same_as<T, Decimal> || std::same_as<T, DecimalView>;

template <class T>
constexpr bool decodable = primitive<T>;

template <class T>
constexpr bool decodable<std::optional<T>> = primitive<T>;

} // namespace detail

/// Checked, borrowed selector. Keep its Row alive and unchanged, including decoded string_views.
class Field {
  const Row *row_ = nullptr;
  std::size_t index_ = 0;
  static_assert(std::is_trivially_copyable_v<std::error_code> && std::is_trivially_destructible_v<std::error_code>);

  // Only failed selectors activate error_; row_ is the discriminator.
  union {
    std::error_code error_;
  };
  friend class Row;

  Field(const Row &row, std::size_t index) noexcept : row_(&row), index_(index)
  {
  }

  const Column &metadata() const noexcept;

  explicit Field(ClientError error) noexcept : error_(make_error_code(error))
  {
  }

  template <class T>
  Result<T> binary_number(std::string_view bytes) const
  {
    const auto type_ = metadata().type;
    const auto flags_ = metadata().flags;
    const bool integer = type_ == 1 || type_ == 2 || type_ == 3 || type_ == 8 || type_ == 9 || type_ == 13;
    if (!integer && type_ != 4 && type_ != 5)
      return std::unexpected(make_error_code(ClientError::invalid_value));
    const std::size_t width = type_ == 1 ? 1 : (type_ == 2 || type_ == 13) ? 2 : (type_ == 8 || type_ == 5) ? 8 : 4;
    if (bytes.size() != width)
      return std::unexpected(make_error_code(ClientError::invalid_value));
    u64 bits = 0;
    if constexpr (std::endian::native == std::endian::little) {
      std::memcpy(&bits, bytes.data(), width);
    } else {
      for (std::size_t index = 0; index < width; ++index)
        bits |= u64{static_cast<unsigned char>(bytes[index])} << (8 * index);
    }

    if (!integer) {
      const auto number = type_ == 4 ? static_cast<f64>(std::bit_cast<f32>(static_cast<u32>(bits)))
                                     : std::bit_cast<f64>(bits);
      if constexpr (!std::floating_point<T>) {
        return std::unexpected(make_error_code(ClientError::invalid_value));
      } else {
        if (!std::isfinite(number) || std::abs(number) > (std::numeric_limits<T>::max)())
          return std::unexpected(make_error_code(ClientError::out_of_range));
        return static_cast<T>(number);
      }
    }

    if (flags_ & 32) {
      if constexpr (std::same_as<T, bool>) {
        if (bits > 1)
          return std::unexpected(make_error_code(ClientError::invalid_value));
      } else if constexpr (std::integral<T>) {
        if (!std::in_range<T>(bits))
          return std::unexpected(make_error_code(ClientError::out_of_range));
      }
      return static_cast<T>(bits);
    }
    if (width < 8 && (bits & (u64{1} << (width * 8 - 1))))
      bits |= (~u64{0}) << (width * 8);
    const auto number = std::bit_cast<i64>(bits);
    if constexpr (std::same_as<T, bool>) {
      if (number != 0 && number != 1)
        return std::unexpected(make_error_code(ClientError::invalid_value));
    } else if constexpr (std::integral<T>) {
      if (!std::in_range<T>(number))
        return std::unexpected(make_error_code(ClientError::out_of_range));
    }
    return static_cast<T>(number);
  }

public:
  /// Decode text/binary primitives with exact/range checks. NULL needs optional<T>.
  /// bool accepts only 0/1; BIT/GEOMETRY remain bytes, Decimal and calendar types preserve vendor semantics.
  /// string owns a copy; string_view borrows the Row. Missing/ambiguous selectors return errors.
  template <class T>
    requires detail::decodable<T>
  Result<T> as() const
  {
    if (!row_)
      return std::unexpected(error_);

    if constexpr (detail::Optional<T>::value) {
      if (*is_null())
        return std::nullopt;

      auto value = as<typename detail::Optional<T>::Type>();
      if (!value)
        return std::unexpected(value.error());
      return T{std::move(*value)};
    } else {
      auto payload = bytes();
      if (!payload)
        return std::unexpected(payload.error());
      const auto text = *payload;
      const auto type_ = metadata().type;
      const auto format_ = format();

      if constexpr (std::integral<T> || std::floating_point<T>) {
        // BIT/GEOMETRY use binary payloads even in a text-protocol result.
        if (type_ == 16 || type_ == 255)
          return std::unexpected(make_error_code(ClientError::invalid_value));
        const bool numeric = type_ == 1 || type_ == 2 || type_ == 3 || type_ == 4 || type_ == 5 || type_ == 8 ||
          type_ == 9 || type_ == 13;
        if (format_ == Format::binary && numeric)
          return binary_number<T>(text);
      }

      if constexpr (std::same_as<T, Date>) {
        return date();
      } else if constexpr (std::same_as<T, DateTime>) {
        return datetime();
      } else if constexpr (std::same_as<T, Time>) {
        return time();
      } else if constexpr (std::same_as<T, Decimal> || std::same_as<T, DecimalView>) {
        if (type_ != 0 && type_ != 246)
          return std::unexpected(make_error_code(ClientError::invalid_value));
        if constexpr (std::same_as<T, Decimal>)
          return Decimal{std::string{text}};
        else
          return DecimalView{text};
      } else if constexpr (std::same_as<T, std::string_view> || std::same_as<T, std::string>) {
        const bool nontext = type_ == 1 || type_ == 2 || type_ == 3 || type_ == 4 || type_ == 5 || type_ == 7 ||
          type_ == 8 || type_ == 9 || type_ == 10 || type_ == 11 || type_ == 12 || type_ == 13;
        if (format_ == Format::binary && nontext)
          return std::unexpected(make_error_code(ClientError::invalid_value));
        if constexpr (std::same_as<T, std::string>)
          return std::string{text};
        else
          return text;
      } else if constexpr (std::same_as<T, bool>) {
        if (text == "0")
          return false;
        if (text == "1")
          return true;
        return std::unexpected(make_error_code(ClientError::invalid_value));
      } else {
        if (text.empty())
          return std::unexpected(make_error_code(ClientError::invalid_value));

        T value{};
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
        if (parsed.ec == std::errc::result_out_of_range)
          return std::unexpected(make_error_code(ClientError::out_of_range));
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
          return std::unexpected(make_error_code(ClientError::invalid_value));
        if constexpr (std::floating_point<T>) {
          if (!std::isfinite(value))
            return std::unexpected(make_error_code(ClientError::invalid_value));
        }
        return value;
      }
    }
  }

  /// Checked NULL inspection, including selection errors.
  Result<bool> is_null() const noexcept;

  /// Borrow exact field payload bytes; use the length, not NUL termination.
  /// NULL is an error; binary numeric bytes are not decimal text.
  Result<std::string_view> bytes() const noexcept;

  Format format() const noexcept;

  /// Decode calendar payloads without timezone conversion or coercing zero dates to NULL.
  Result<Date> date() const;
  Result<DateTime> datetime() const;
  Result<Time> time() const;
};

static_assert(std::is_trivially_copyable_v<Field> && std::is_trivially_destructible_v<Field>);

/// Owning text/binary row retaining its immutable schema, even after the result/session dies.
/// Fields and string_views borrow the Row; copying a Row copies values and shares metadata.
/// Buffered rows retain a shared result pool; streamed rows own independent value pools.
class Row {
  struct Cell {
    u32 offset = 0;
    u32 size = std::numeric_limits<u32>::max();
  };

  using Columns = db::detail::ResultList<Column>;
  std::shared_ptr<const Columns> columns_;
  // One inline cell; additional offset/length descriptors precede the owned wire payload.
  db::detail::ResultText storage_;
  Cell first_;
  friend class Connection;
  friend class Field;
  Format format_ = Format::text;

  Cell cell(std::size_t index) const noexcept
  {
    if (!index)
      return first_;
    Cell value;
    std::memcpy(&value, storage_.data() + (index - 1) * sizeof(Cell), sizeof(Cell));
    return value;
  }

  void cell(std::size_t index, Cell value) noexcept
  {
    if (!index)
      first_ = value;
    else
      std::memcpy(storage_.data() + (index - 1) * sizeof(Cell), &value, sizeof(Cell));
  }

public:
  Row() = default;

  /// Number of columns, not rows.
  std::size_t size() const noexcept
  {
    return columns_ ? columns_->size() : 0;
  }

  /// Select a zero-based column; failed selection is reported by the Field conversion.
  Field operator[](std::size_t index) const & noexcept
  {
    if (index >= size())
      return Field{ClientError::missing_column};
    return Field{*this, index};
  }

  /// Select an exact case-sensitive name; duplicate names are errors, not first-match wins.
  Field operator[](std::string_view name) const & noexcept;
  Field operator[](std::size_t) const && = delete;
  Field operator[](std::string_view) const && = delete;

  /// Synchronously map member names to columns using shared db reflection/optional RowTraits.
  /// Owning members copy; string_view members still borrow this Row.
  template <class T>
  Result<T> as() const &;
  template <class T>
  Result<T> as() const && = delete;
};

inline const Column &Field::metadata() const noexcept
{
  return (*row_->columns_)[index_];
}

inline Result<bool> Field::is_null() const noexcept
{
  if (!row_)
    return std::unexpected(error_);
  return row_->cell(index_).size == std::numeric_limits<u32>::max();
}

inline Result<std::string_view> Field::bytes() const noexcept
{
  if (!row_)
    return std::unexpected(error_);
  const auto value = row_->cell(index_);
  if (value.size == std::numeric_limits<u32>::max())
    return std::unexpected(make_error_code(ClientError::null_value));
  const auto descriptors = (row_->size() - 1) * sizeof(Row::Cell);
  return std::string_view{row_->storage_.data() + descriptors + value.offset, value.size};
}

inline Format Field::format() const noexcept
{
  return row_ ? row_->format_ : Format::text;
}

/// Owned single-command response. Row data survives session close; selected Fields do not own rows.
struct ResultSet {
  db::detail::ResultList<Column> columns;
  db::detail::ResultList<Row> rows;
  u64 affected_rows = 0;
  u64 last_insert_id = 0;
  u16 warnings = 0;
  u16 status = 0;
  db::detail::ResultText info;
  Format format = Format::text;
};

using Results = db::detail::ResultList<ResultSet>;

} // namespace weave::mysql
