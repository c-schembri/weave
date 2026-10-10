#pragma once

#include <weave/task.hpp>
#include <atomic>
#include <algorithm>
#include <cstddef>
#include <concepts>
#include <cstring>
#include <cstdlib>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace weave::pg::detail {

struct ResultHeap {
  void *state = nullptr;
  void *(*allocate)(void *, std::size_t, std::size_t) noexcept;
  void (*deallocate)(void *, void *, std::size_t, std::size_t) noexcept;

  static void *reserve(void *, std::size_t size, std::size_t alignment) noexcept
  {
    return ::operator new(size, std::align_val_t{alignment}, std::nothrow);
  }

  static void release(void *, void *data, std::size_t, std::size_t alignment) noexcept
  {
    ::operator delete(data, std::align_val_t{alignment});
  }

  static ResultHeap standard() noexcept
  {
    return {nullptr, reserve, release};
  }
};

using weave::detail::require;

class ResultStorage {
  struct Block {
    Block *next = nullptr;
    ResultStorage *owner = nullptr;
    std::size_t size = 0;
    std::size_t alignment = 0;
    bool active = true;
  };

  ResultHeap heap_;
  std::atomic<std::size_t> references_{1};
  std::atomic<std::size_t> bytes_{sizeof(ResultStorage)};
  std::mutex mutex_;
  Block *blocks_ = nullptr;

  explicit ResultStorage(ResultHeap heap) noexcept : heap_(heap)
  {
  }

  ~ResultStorage()
  {
    while (blocks_) {
      auto *block = std::exchange(blocks_, blocks_->next);
      require(!block->active);
      const auto size = block->size;
      const auto alignment = block->alignment;
      std::destroy_at(block);
      heap_.deallocate(heap_.state, block, size, alignment);
    }
  }

public:
  static ResultStorage *create(ResultHeap heap)
  {
    require(heap.allocate != nullptr && heap.deallocate != nullptr);
    auto *memory = heap.allocate(heap.state, sizeof(ResultStorage), alignof(ResultStorage));
    require(memory != nullptr);
    return ::new (memory) ResultStorage(heap);
  }

  void retain() noexcept
  {
    auto count = references_.fetch_add(1, std::memory_order_relaxed);
    require(count && count != std::numeric_limits<std::size_t>::max());
  }

  void release() noexcept
  {
    auto count = references_.fetch_sub(1, std::memory_order_acq_rel);
    require(count != 0);
    if (count != 1)
      return;

    const auto heap = heap_;
    this->~ResultStorage();
    heap.deallocate(heap.state, this, sizeof(ResultStorage), alignof(ResultStorage));
  }

  std::size_t bytes() const noexcept
  {
    return bytes_.load(std::memory_order_relaxed);
  }

  ResultHeap heap() const noexcept
  {
    return heap_;
  }

  void *allocate(std::size_t size, std::size_t alignment)
  {
    require(alignment && (alignment & (alignment - 1)) == 0);
    alignment = std::max(alignment, alignof(Block));
    const auto maximum = std::numeric_limits<std::size_t>::max();
    require(alignment - 1 <= maximum - sizeof(Block) - sizeof(Block *));
    const auto overhead = sizeof(Block) + sizeof(Block *) + alignment - 1;
    const auto prefix = overhead & ~(alignment - 1);
    require(size <= maximum - prefix);
    const auto requested = prefix + size;

    auto *memory = heap_.allocate(heap_.state, requested, alignment);
    require(memory != nullptr);
    auto *block = ::new (memory) Block;
    block->owner = this;
    block->size = requested;
    block->alignment = alignment;
    auto *data = static_cast<std::byte *>(memory) + prefix;
    std::memcpy(data - sizeof(Block *), &block, sizeof(block));

    retain();
    {
      std::lock_guard lock{mutex_};
      const auto current = bytes_.load(std::memory_order_relaxed);
      require(requested <= maximum - current);
      bytes_.store(current + requested, std::memory_order_relaxed);
      block->next = std::exchange(blocks_, block);
    }
    return data;
  }

  static void deallocate(void *data) noexcept
  {
    Block *block = nullptr;
    std::memcpy(&block, static_cast<std::byte *>(data) - sizeof(Block *), sizeof(block));
    require(block && block->active);
    block->active = false;
    // Keep actual blocks until the arena's last owner dies, as a result pool.
    // An allocation also owns the arena so rebound/debug-proxy frees are safe.
    block->owner->release();
  }
};

class ResultArena {
  ResultStorage *storage_ = nullptr;

  explicit ResultArena(ResultStorage *storage) noexcept : storage_(storage)
  {
  }

public:
  ResultArena() noexcept = default;

  static ResultArena create(ResultHeap heap = ResultHeap::standard())
  {
    return ResultArena{ResultStorage::create(heap)};
  }

  ResultArena(const ResultArena &other) noexcept : storage_(other.storage_)
  {
    if (storage_)
      storage_->retain();
  }

  ResultArena(ResultArena &&other) noexcept : storage_(std::exchange(other.storage_, nullptr))
  {
  }

  ResultArena &operator=(const ResultArena &other) noexcept
  {
    ResultArena replacement{other};
    swap(replacement);
    return *this;
  }

  ResultArena &operator=(ResultArena &&other) noexcept
  {
    if (this != &other) {
      ResultArena replacement{std::move(other)};
      swap(replacement);
    }
    return *this;
  }

  ~ResultArena()
  {
    if (storage_)
      storage_->release();
  }

  void swap(ResultArena &other) noexcept
  {
    std::swap(storage_, other.storage_);
  }

  const void *identity() const noexcept
  {
    return storage_;
  }

  std::size_t bytes() const noexcept
  {
    return storage_ ? storage_->bytes() : 0;
  }

  ResultHeap heap() const noexcept
  {
    return storage_ ? storage_->heap() : ResultHeap::standard();
  }

  ResultArena fresh() const
  {
    return storage_ ? create(storage_->heap()) : ResultArena{};
  }

  void *allocate(std::size_t size, std::size_t alignment)
  {
    if (!storage_)
      *this = create();
    return storage_->allocate(size, alignment);
  }
};

template <class T>
class ResultAllocator {
  ResultArena arena_;

public:
  using value_type = T;
  using propagate_on_container_copy_assignment = std::false_type;
  using propagate_on_container_move_assignment = std::true_type;
  using propagate_on_container_swap = std::true_type;
  using is_always_equal = std::false_type;

  ResultAllocator() noexcept : ResultAllocator(ResultArena{})
  {
  }

  ResultAllocator(const ResultAllocator &) noexcept = default;

  // Preserve the source's arena too: standard-library proxy/iterator storage
  // can remain in a moved-from container until that container is reset.
  ResultAllocator(ResultAllocator &&other) noexcept : arena_(other.arena_)
  {
  }

  ResultAllocator &operator=(const ResultAllocator &) noexcept = default;

  ResultAllocator &operator=(ResultAllocator &&other) noexcept
  {
    arena_ = other.arena_;
    return *this;
  }

  explicit ResultAllocator(ResultArena arena) noexcept : arena_(std::move(arena))
  {
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL > 0
    // MSVC's empty containers allocate iterator proxies through a rebound copy.
    // Establish shared storage before that copy so its requests remain visible.
    if (!arena_.identity())
      arena_ = ResultArena::create();
#endif
  }

  template <class U>
  ResultAllocator(const ResultAllocator<U> &other) noexcept : arena_(other.arena())
  {
  }

  T *allocate(std::size_t count)
  {
    require(count <= std::numeric_limits<std::size_t>::max() / sizeof(T));
    return static_cast<T *>(arena_.allocate(count * sizeof(T), alignof(T)));
  }

  void deallocate(T *data, std::size_t) noexcept
  {
    ResultStorage::deallocate(data);
  }

  const ResultArena &arena() const noexcept
  {
    return arena_;
  }

  ResultAllocator select_on_container_copy_construction() const
  {
    return ResultAllocator{arena_.fresh()};
  }

  template <class U>
  bool operator==(const ResultAllocator<U> &other) const noexcept
  {
    return arena_.identity() == other.arena().identity();
  }
};

class ResultText : public std::basic_string<char, std::char_traits<char>, ResultAllocator<char>> {
  using Base = std::basic_string<char, std::char_traits<char>, ResultAllocator<char>>;

public:
  using Base::Base;
  using Base::operator=;
  ResultText() = default;
  ResultText(const ResultText &) = default;
  ResultText(ResultText &&) noexcept = default;
  ResultText &operator=(const ResultText &) = default;
  ResultText &operator=(ResultText &&) noexcept = default;

  ResultText(std::string_view text) : Base(text.empty() ? "" : text.data(), text.size())
  {
  }

  ResultText(const std::string &text) : ResultText(std::string_view{text})
  {
  }

  explicit operator std::string() const
  {
    return std::string{data(), size()};
  }

  std::string substr(std::size_t offset = 0, std::size_t count = Base::npos) const
  {
    return std::string{std::string_view{*this}.substr(offset, count)};
  }

  template <class Left, class Right>
    requires(
      (std::same_as<Left, ResultText> || std::same_as<Right, ResultText>) &&
      std::convertible_to<const Left &, std::string_view> && std::convertible_to<const Right &, std::string_view>)
  friend std::string operator+(const Left &left, const Right &right)
  {
    std::string result{std::string_view{left}};
    result.append(std::string_view{right});
    return result;
  }

  friend std::string operator+(const ResultText &text, char character)
  {
    std::string result{std::string_view{text}};
    result.push_back(character);
    return result;
  }

  friend std::string operator+(char character, const ResultText &text)
  {
    std::string result(1, character);
    result.append(std::string_view{text});
    return result;
  }

  friend bool operator==(const ResultText &, const ResultText &) = default;

  friend bool operator==(const ResultText &left, std::string_view right) noexcept
  {
    return std::string_view{left} == right;
  }

  friend bool operator==(const ResultText &left, const std::string &right) noexcept
  {
    return left == std::string_view{right};
  }

  friend bool operator==(const ResultText &left, const char *right) noexcept
  {
    return left == std::string_view{right};
  }
};

template <class T>
class ResultList : public std::vector<T, ResultAllocator<T>> {
  using Base = std::vector<T, ResultAllocator<T>>;

public:
  using Base::Base;
  using Base::operator=;
  ResultList() = default;
  ResultList(const ResultList &) = default;
  ResultList(ResultList &&) noexcept = default;
  ResultList &operator=(const ResultList &) = default;
  ResultList &operator=(ResultList &&) noexcept = default;

  ResultList(const std::vector<T> &values) : Base(values.begin(), values.end())
  {
  }

  ResultList(std::vector<T> &&values)
      : Base(std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()))
  {
  }

  ResultList &operator=(const std::vector<T> &values)
  {
    this->assign(values.begin(), values.end());
    return *this;
  }

  ResultList &operator=(std::vector<T> &&values)
  {
    this->assign(std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
    return *this;
  }

  explicit operator std::vector<T>() const
  {
    return std::vector<T>{this->begin(), this->end()};
  }

  friend bool operator==(const ResultList &, const ResultList &) = default;

  friend bool operator==(const ResultList &left, const std::vector<T> &right)
  {
    return std::ranges::equal(left, right);
  }
};

} // namespace weave::pg::detail

namespace std {

template <>
struct hash<weave::pg::detail::ResultText> {
  size_t operator()(const weave::pg::detail::ResultText &text) const noexcept
  {
    return hash<string_view>{}(string_view{text});
  }
};

template <>
struct formatter<weave::pg::detail::ResultText, char> : formatter<string_view, char> {
  template <class FormatContext>
  auto format(const weave::pg::detail::ResultText &text, FormatContext &context) const
  {
    return formatter<string_view, char>::format(string_view{text}, context);
  }
};

} // namespace std
