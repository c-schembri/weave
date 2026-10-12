#pragma once

#include <weave/task.hpp>
#include <glaze/json/read.hpp>
#include <glaze/json/write.hpp>
#include <glaze/core/error_category.hpp>
#include <span>

namespace weave::json {

/// Owning diagnostic snapshot; offsets are zero-based bytes. Messages never include the input document.
struct Failure {
  std::error_code error;
  std::size_t offset = 0;
  std::string message;
};

namespace detail {

struct ReadPolicy : glz::opts {
  bool null_terminated = false;
  bool validate_skipped = true;
  bool validate_trailing_whitespace = true;
  bool validate_utf8 = true;
};

class Category final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.json";
  }

  std::string message(int value) const override
  {
    if (value < 0 || static_cast<std::size_t>(value) >= glz::meta<glz::error_code>::keys.size())
      return "Unknown JSON error";
    return std::string{glz::meta<glz::error_code>::keys[static_cast<std::size_t>(value)]};
  }
};

inline Result<void> status(glz::error_ctx error, Failure *failure)
{
  if (failure)
    *failure = {};
  if (!error)
    return {};

  static Category category;
  std::error_code code{static_cast<int>(error.ec), category};
  if (failure)
    *failure = {code, error.count, code.message()};
  return std::unexpected(code);
}

} // namespace detail

/// Synchronously decode a complete UTF-8 document into existing storage; failure may leave partial updates.
/// Unknown keys fail; absent keys retain existing values. Glaze customization is supported without Weave metadata.
/// Views into input remain borrowed. This does not add protocol-specific duplicate-key or allocation budgets.
/// Input must not alias storage that decoding modifies.
template <class T>
Result<void> read(T &value, std::string_view input, Failure *failure = nullptr)
{
  return detail::status(glz::read<detail::ReadPolicy{}>(value, input), failure);
}

/// Decode into a new value-initialized object. All work is synchronous; borrowed fields still borrow input.
template <class T>
Result<T> read(std::string_view input, Failure *failure = nullptr)
{
  T value{};
  auto parsed = read(value, input, failure);
  if (!parsed)
    return std::unexpected(parsed.error());
  return value;
}

/// Replace a caller-owned string, reusing its capacity. Failure may leave a partial document.
/// Output must not alias value or its borrowed data.
template <class T>
Result<void> write(const T &value, std::string &buffer, Failure *failure = nullptr)
{
  return detail::status(glz::write_json(value, buffer), failure);
}

/// Write into a fixed caller-owned buffer and return bytes written; no terminating NUL is added.
/// Insufficient capacity fails rather than allocating a larger output. Failed output is not a valid document.
/// Output must not alias value or its borrowed data.
template <class T>
Result<std::size_t> write(const T &value, std::span<char> buffer, Failure *failure = nullptr)
{
  auto written = glz::write_json(value, buffer);
  auto checked = detail::status(written, failure);
  if (!checked)
    return std::unexpected(checked.error());
  return written.count;
}

/// Serialize to an owning string; use the buffer overloads when output allocation must be controlled.
template <class T>
Result<std::string> write(const T &value, Failure *failure = nullptr)
{
  std::string buffer;
  auto written = write(value, buffer, failure);
  if (!written)
    return std::unexpected(written.error());
  return buffer;
}

} // namespace weave::json
