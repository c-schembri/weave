#pragma once

#include <array>
#include <memory>
#include <type_traits>
#include <vector>

namespace weave::pg::detail {

void clear_secret(void *data, std::size_t size) noexcept;

// Authentication storage cleanses the complete allocation on growth and release.
template <class T>
struct SecretAllocator {
  static_assert(std::is_trivially_copyable_v<T>);
  using value_type = T;
  using is_always_equal = std::true_type;

  SecretAllocator() noexcept = default;

  template <class U>
  SecretAllocator(const SecretAllocator<U> &) noexcept
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
  bool operator==(const SecretAllocator<U> &) const noexcept
  {
    return true;
  }
};

template <class T>
using SecretStorage = std::vector<T, SecretAllocator<T>>;

using SecretText = SecretStorage<char>;

// Fixed cryptographic intermediates are move-only; a move also clears its source.
template <std::size_t Size>
struct SecretArray {
  std::array<unsigned char, Size> bytes{};

  SecretArray() noexcept = default;
  SecretArray(const SecretArray &) = delete;

  SecretArray(SecretArray &&other) noexcept : bytes(other.bytes)
  {
    clear_secret(other.bytes.data(), other.bytes.size());
  }

  SecretArray &operator=(SecretArray &&other) noexcept
  {
    if (this != &other) {
      clear_secret(bytes.data(), bytes.size());
      bytes = other.bytes;
      clear_secret(other.bytes.data(), other.bytes.size());
    }
    return *this;
  }

  ~SecretArray()
  {
    clear_secret(bytes.data(), bytes.size());
  }
};

} // namespace weave::pg::detail
