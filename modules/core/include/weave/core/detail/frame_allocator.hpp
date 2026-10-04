#pragma once

#include <weave/types.hpp>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define WEAVE_DETAIL_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(WEAVE_DETAIL_ASAN)
#define WEAVE_DETAIL_ASAN 1
#endif
#if defined(WEAVE_DETAIL_ASAN)
#include <sanitizer/asan_interface.h>
#endif

#ifndef WEAVE_RECYCLE_FRAMES
#define WEAVE_RECYCLE_FRAMES 1
#endif

namespace weave::detail {

struct FrameMetrics {
  u64 allocations = 0;
  u64 bytes = 0;
  u64 heap_allocations = 0;
  u64 heap_bytes = 0;
  u64 cache_hits = 0;
};

inline thread_local FrameMetrics frame_metrics;

inline void *frame_heap_allocate(std::size_t size, std::size_t alignment)
{
#if defined(WEAVE_PROFILE_RUNTIME)
  ++frame_metrics.heap_allocations;
  frame_metrics.heap_bytes += size;
#endif
  auto *memory = alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__
    ? ::operator new(size, std::align_val_t{alignment}, std::nothrow)
    : ::operator new(size, std::nothrow);
  if (!memory)
    std::abort();

  return memory;
}

inline void frame_heap_free(void *memory, std::size_t alignment) noexcept
{
  if (alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
    ::operator delete(memory, std::align_val_t{alignment});
  else
    ::operator delete(memory);
}

// Only dead frames enter the cache. No frame points back to its allocating thread.
class FrameCache {
  struct Node {
    Node *next;
  };

  struct Bucket {
    Node *head = nullptr;
    std::size_t count = 0;
  };

  std::array<Bucket, 4> buckets_{};

  static std::size_t index(std::size_t size) noexcept
  {
    return size == 0 ? 0 : std::bit_width((size - 1) >> 7);
  }

  static void unpoison(void *memory, std::size_t size) noexcept
  {
#if defined(WEAVE_DETAIL_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(memory, size);
#else
    (void)memory;
    (void)size;
#endif
  }

  static void poison(void *memory, std::size_t size) noexcept
  {
#if defined(WEAVE_DETAIL_ASAN)
    ASAN_POISON_MEMORY_REGION(memory, size);
#else
    (void)memory;
    (void)size;
#endif
  }

public:
  static constexpr std::size_t max_size = 1024;
  static constexpr std::size_t bucket_limit = 64;

  static std::size_t allocation_size(std::size_t size, std::size_t alignment) noexcept
  {
    auto cacheable = size <= max_size && alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    return cacheable ? std::size_t{128} << index(size) : size;
  }

  FrameCache() = default;
  FrameCache(const FrameCache &) = delete;
  FrameCache &operator=(const FrameCache &) = delete;

  ~FrameCache()
  {
    clear();
  }

  void *allocate(std::size_t size, std::size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__)
  {
    if (size > max_size || alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
      return frame_heap_allocate(size, alignment);

    const auto i = index(size);
    const auto capacity = std::size_t{128} << i;
    auto &bucket = buckets_[i];
    void *memory;

    if (bucket.head) {
      memory = bucket.head;
      unpoison(memory, capacity);
      bucket.head = bucket.head->next;
      --bucket.count;
#if defined(WEAVE_PROFILE_RUNTIME)
      ++frame_metrics.cache_hits;
#endif
    } else {
      memory = frame_heap_allocate(capacity, alignment);
    }

    poison(static_cast<std::byte *>(memory) + size, capacity - size);
    return memory;
  }

  void deallocate(void *memory, std::size_t size, std::size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__) noexcept
  {
    if (size <= max_size && alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
      const auto i = index(size);
      const auto capacity = std::size_t{128} << i;
      auto &bucket = buckets_[i];
      unpoison(memory, capacity);

      if (bucket.count < bucket_limit) {
        bucket.head = ::new (memory) Node{bucket.head};
        ++bucket.count;
        poison(memory, capacity);
        return;
      }
    }

    frame_heap_free(memory, alignment);
  }

  std::size_t cached_frames() const noexcept
  {
    std::size_t count = 0;
    for (const auto &bucket : buckets_)
      count += bucket.count;

    return count;
  }

  void clear() noexcept
  {
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
      auto &bucket = buckets_[i];
      while (bucket.head) {
        auto *memory = bucket.head;
        unpoison(memory, std::size_t{128} << i);
        bucket.head = memory->next;
        ::operator delete(memory);
      }

      bucket.count = 0;
    }
  }
};

inline constinit thread_local bool frame_cache_stopped = false;

struct ThreadFrameCache : FrameCache {
  ~ThreadFrameCache()
  {
    frame_cache_stopped = true;
  }
};

inline thread_local ThreadFrameCache frame_cache;

inline void *allocate_frame(std::size_t size, std::size_t alignment)
{
#if defined(WEAVE_PROFILE_RUNTIME)
  ++frame_metrics.allocations;
  frame_metrics.bytes += size;
#endif
#if WEAVE_RECYCLE_FRAMES
  // Other TLS destructors may still create/destroy coroutines after cache teardown.
  if (!frame_cache_stopped)
    return frame_cache.allocate(size, alignment);

  // A frame created during TLS teardown may later be freed on another thread.
  size = FrameCache::allocation_size(size, alignment);
#endif
  return frame_heap_allocate(size, alignment);
}

inline void deallocate_frame(void *memory, std::size_t size, std::size_t alignment) noexcept
{
#if WEAVE_RECYCLE_FRAMES
  if (!frame_cache_stopped) {
    frame_cache.deallocate(memory, size, alignment);
    return;
  }
#else
  (void)size;
#endif
  frame_heap_free(memory, alignment);
}

} // namespace weave::detail
