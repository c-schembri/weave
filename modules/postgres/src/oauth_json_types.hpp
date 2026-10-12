#pragma once

#include "secret.hpp"
#include <weave/json.hpp>
#include <map>

namespace weave::pg::detail {

// No inline string storage: decoded keys and values cleanse their complete allocations on release.
class OAuthJsonText {
  SecretText bytes_;

public:
  using value_type = char;

  operator std::string_view() const noexcept
  {
    return {bytes_.data(), bytes_.size()};
  }

  char *data() noexcept
  {
    return bytes_.data();
  }

  const char *data() const noexcept
  {
    return bytes_.data();
  }

  std::size_t size() const noexcept
  {
    return bytes_.size();
  }

  void resize(std::size_t size)
  {
    bytes_.resize(size);
  }

  void append(const char *data, std::size_t size)
  {
    bytes_.insert(bytes_.end(), data, data + size);
  }

  void assign(const char *data, std::size_t size)
  {
    bytes_.assign(data, data + size);
  }
};

struct OAuthJsonValue;

// Container records include numeric values; clear those allocations after their nontrivial elements are destroyed.
template <class T>
struct OAuthJsonAllocator {
  using value_type = T;
  using is_always_equal = std::true_type;

  OAuthJsonAllocator() noexcept = default;

  template <class U>
  OAuthJsonAllocator(const OAuthJsonAllocator<U> &) noexcept
  {
  }

  T *allocate(std::size_t count)
  {
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T *data, std::size_t count) noexcept
  {
    clear_secret(data, count * sizeof(T));
    std::allocator<T>{}.deallocate(data, count);
  }

  template <class U>
  bool operator==(const OAuthJsonAllocator<U> &) const noexcept
  {
    return true;
  }
};

struct OAuthJsonLess {
  using is_transparent = void;

  bool operator()(std::string_view left, std::string_view right) const noexcept
  {
    return left < right;
  }
};

using OAuthJsonEntry = std::pair<const OAuthJsonText, OAuthJsonValue>;
using OAuthJsonMapBase = std::map<OAuthJsonText, OAuthJsonValue, OAuthJsonLess, OAuthJsonAllocator<OAuthJsonEntry>>;

struct OAuthJsonMap : OAuthJsonMapBase {
  bool duplicate = false;
  std::size_t members = 0;

  OAuthJsonValue &operator[](const OAuthJsonText &key);
};

struct OAuthJsonValue {
  using Array = std::vector<OAuthJsonValue, OAuthJsonAllocator<OAuthJsonValue>>;
  using Object = OAuthJsonMap;
  std::variant<std::nullptr_t, u64, i64, f64, OAuthJsonText, bool, Array, Object> data;
  bool unsigned_integer_token = false;

  OAuthJsonValue() = default;
  OAuthJsonValue(const OAuthJsonValue &) = default;
  OAuthJsonValue(OAuthJsonValue &&) noexcept = default;
  OAuthJsonValue &operator=(const OAuthJsonValue &) = default;
  OAuthJsonValue &operator=(OAuthJsonValue &&) noexcept = default;

  ~OAuthJsonValue()
  {
    std::visit(
      []<class T>(T &value) {
        if constexpr (std::is_arithmetic_v<T> || std::same_as<T, std::nullptr_t>)
          clear_secret(&value, sizeof(value));
      },
      data);
  }
};

inline OAuthJsonValue &OAuthJsonMap::operator[](const OAuthJsonText &key)
{
  ++members;
  auto existing = find(std::string_view{key});
  if (existing != end()) {
    duplicate = true;
    return existing->second;
  }
  return OAuthJsonMapBase::operator[](key);
}

struct OAuthJsonContext : glz::context {
  std::size_t nodes = 0;
};

} // namespace weave::pg::detail

// These private named types need Glaze customization hooks; the JSON grammar remains entirely upstream.
template <>
struct glz::meta<weave::pg::detail::OAuthJsonValue> {
  static constexpr bool custom_read = true;
};

template <>
struct glz::from<glz::JSON, weave::pg::detail::OAuthJsonValue> {
  template <auto Opts>
  static void op(weave::pg::detail::OAuthJsonValue &value, glz::is_context auto &ctx, auto &it, auto end)
  {
    if (++ctx.nodes > 4096) {
      ctx.error = glz::error_code::exceeded_max_recursive_depth;
      return;
    }
    if (glz::skip_ws<Opts>(ctx, it, end))
      return;
    if (it == end) {
      ctx.error = glz::error_code::unexpected_end;
      return;
    }
    if ((*it == '{' || *it == '[') && ctx.depth >= 8) {
      ctx.error = glz::error_code::exceeded_max_recursive_depth;
      return;
    }
    const auto start = it;
    value.unsigned_integer_token = false;
    glz::parse<glz::JSON>::op<Opts>(value.data, ctx, it, end);
    if (!glz::parse_failed(ctx.error) && std::holds_alternative<weave::u64>(value.data)) {
      // Glaze permits integral exponent notation; OAuth requires an unsigned integer-form token.
      const std::string_view token{start, static_cast<std::size_t>(it - start)};
      value.unsigned_integer_token = token.front() != '-' && token.find_first_of(".eE") == std::string_view::npos;
    }
  }
};
