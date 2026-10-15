#pragma once

#include <weave/mysql/error.hpp>
#include <span>
#include <variant>
#include <string>
#include <vector>

namespace weave::mysql {

/// Calendar components without implicit timezone conversion. Zero dates remain representable.
struct Date {
  u16 year = 0;
  u8 month = 0;
  u8 day = 0;
};

/// Calendar date/time with microseconds, without a timezone or implicit UTC conversion.
struct DateTime {
  Date date;
  u8 hour = 0;
  u8 minute = 0;
  u8 second = 0;
  u32 microsecond = 0;
};

/// MySQL TIME may be negative or longer than a day; it is not a wall-clock timestamp.
struct Time {
  bool negative = false;
  u32 days = 0;
  u8 hour = 0;
  u8 minute = 0;
  u8 second = 0;
  u32 microsecond = 0;
};

/// Exact decimal parameter, never an intermediate floating-point conversion.
struct Decimal {
  std::string value;
};

/// Borrowed exact decimal text; retain it unchanged through the consuming operation.
struct DecimalView {
  std::string_view value;
};

/// Inline scalar or borrowed text/blob/decimal parameter. Construction does not allocate.
/// Keep referenced bytes unchanged through the consuming Task's completion/cancellation cleanup.
class ParameterView {
public:
  using Value = std::variant<
    std::monostate,
    bool,
    i64,
    u64,
    f32,
    f64,
    std::string_view,
    std::span<const std::byte>,
    Date,
    DateTime,
    Time,
    DecimalView>;
  Value value;

  ParameterView() = default;

  ParameterView(std::nullopt_t) noexcept
  {
  }

  ParameterView(bool input) noexcept : value(input)
  {
  }

  template <std::integral T>
    requires(!std::same_as<T, bool> && sizeof(T) <= 8)
  ParameterView(T input) noexcept
  {
    if constexpr (std::is_signed_v<T>)
      value = static_cast<i64>(input);
    else
      value = static_cast<u64>(input);
  }

  ParameterView(f32 input) noexcept : value(input)
  {
  }

  ParameterView(f64 input) noexcept : value(input)
  {
  }

  ParameterView(std::string_view input) noexcept : value(input)
  {
  }

  ParameterView(const char *input) noexcept : value(input ? Value{std::string_view{input}} : Value{})
  {
  }

  ParameterView(std::span<const std::byte> input) noexcept : value(input)
  {
  }

  ParameterView(Date input) noexcept : value(input)
  {
  }

  ParameterView(DateTime input) noexcept : value(input)
  {
  }

  ParameterView(Time input) noexcept : value(input)
  {
  }

  ParameterView(DecimalView input) noexcept : value(input)
  {
  }
};

/// Owned parameter snapshot. Text/blob inputs copy; moved strings/vectors transfer their storage.
/// Numeric/calendar values are inline. NULL and an empty string/blob remain distinct.
class Parameter {
public:
  using Value = std::variant<
    std::monostate,
    bool,
    i64,
    u64,
    f32,
    f64,
    std::string,
    std::vector<std::byte>,
    Date,
    DateTime,
    Time,
    Decimal>;
  Value value;

  Parameter() = default;

  Parameter(std::nullopt_t) noexcept
  {
  }

  Parameter(bool input) noexcept : value(input)
  {
  }

  template <std::integral T>
    requires(!std::same_as<T, bool> && sizeof(T) <= 8)
  Parameter(T input) noexcept
  {
    if constexpr (std::is_signed_v<T>)
      value = static_cast<i64>(input);
    else
      value = static_cast<u64>(input);
  }

  Parameter(f32 input) noexcept : value(input)
  {
  }

  Parameter(f64 input) noexcept : value(input)
  {
  }

  Parameter(std::string input) : value(std::move(input))
  {
  }

  Parameter(std::string_view input) : value(std::string{input})
  {
  }

  Parameter(const char *input) : value(input ? Value{std::string{input}} : Value{})
  {
  }

  Parameter(std::vector<std::byte> input) : value(std::move(input))
  {
  }

  Parameter(std::span<const std::byte> input) : value(std::vector<std::byte>{input.begin(), input.end()})
  {
  }

  Parameter(Date input) noexcept : value(input)
  {
  }

  Parameter(DateTime input) noexcept : value(input)
  {
  }

  Parameter(Time input) noexcept : value(input)
  {
  }

  Parameter(Decimal input) : value(std::move(input))
  {
  }

  /// Borrow this snapshot's variable-size values; never use the view after mutation/destruction.
  ParameterView view() const & noexcept
  {
    return std::visit(
      []<class T>(const T &input) -> ParameterView {
        if constexpr (std::same_as<T, std::monostate>)
          return {};
        else if constexpr (std::same_as<T, std::string>)
          return std::string_view{input};
        else if constexpr (std::same_as<T, std::vector<std::byte>>)
          return std::span<const std::byte>{input};
        else if constexpr (std::same_as<T, Decimal>)
          return DecimalView{input.value};
        else
          return input;
      },
      value);
  }

  ParameterView view() const && = delete;
};

} // namespace weave::mysql
