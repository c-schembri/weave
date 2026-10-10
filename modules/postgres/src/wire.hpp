#pragma once

#include <weave/postgres/connection.hpp>
#include <algorithm>
#include <limits>

namespace weave::pg::detail {

using Bytes = std::vector<std::byte>;

inline bool cstring_valid(std::string_view value) noexcept
{
  return value.find('\0') == std::string_view::npos;
}

class Reader {
  std::span<const std::byte> bytes_;
  bool valid_ = true;

public:
  explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes)
  {
  }

  std::span<const std::byte> take(std::size_t size) noexcept
  {
    if (!valid_ || size > bytes_.size()) {
      valid_ = false;
      return {};
    }

    auto value = bytes_.first(size);
    bytes_ = bytes_.subspan(size);
    return value;
  }

  u32 integer(std::size_t size = 4) noexcept
  {
    u32 value = 0;
    for (auto byte : take(size))
      value = (value << 8) | std::to_integer<u8>(byte);

    return value;
  }

  std::string string()
  {
    auto end = std::find(bytes_.begin(), bytes_.end(), std::byte{});
    if (!valid_ || end == bytes_.end()) {
      valid_ = false;
      return {};
    }

    auto size = static_cast<std::size_t>(end - bytes_.begin());
    auto value = take(size + 1);
    return {reinterpret_cast<const char *>(value.data()), size};
  }

  bool empty() const noexcept
  {
    return valid_ && bytes_.empty();
  }

  bool valid() const noexcept
  {
    return valid_;
  }

  std::span<const std::byte> rest() noexcept
  {
    return take(bytes_.size());
  }
};

template <class Storage>
class BasicWriter {
public:
  Storage bytes;

  void integer(u32 value, std::size_t size = 4)
  {
    for (auto shift = size; shift > 0; --shift)
      bytes.push_back(static_cast<std::byte>((value >> ((shift - 1) * 8)) & 255));
  }

  void raw(std::span<const std::byte> value)
  {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }

  void raw(std::string_view value)
  {
    raw(std::as_bytes(std::span{value.data(), value.size()}));
  }

  void string(std::string_view value)
  {
    raw(value);
    integer(0, 1);
  }

  template <class BodyStorage>
  void message(char kind, const BasicWriter<BodyStorage> &body)
  {
    integer(static_cast<u8>(kind), 1);
    integer(static_cast<u32>(body.bytes.size() + 4));
    raw(std::span{body.bytes});
  }

  void message(char kind)
  {
    message(kind, BasicWriter{});
  }
};

using Writer = BasicWriter<Bytes>;

struct Message {
  char kind = 0;
  Bytes body;
};

Result<Diagnostic> diagnostic(std::span<const std::byte> body);
Result<std::vector<Column>> columns(std::span<const std::byte> body, ResultArena arena = {});
Result<Row> row(std::span<const std::byte> body, std::size_t columns, ResultArena arena = {});
Result<Row> row(std::span<const std::byte> body, std::span<const Column> columns, ResultArena arena = {});
Result<Notification> notification(std::span<const std::byte> body);
Result<void> bind(
  Writer &request,
  std::string_view name,
  const std::vector<Parameter> &parameters,
  Format format,
  std::size_t limit,
  std::string_view portal = {});

Result<Writer> function_call(u32 function, const std::vector<Parameter> &parameters, Format format, std::size_t limit);

} // namespace weave::pg::detail
