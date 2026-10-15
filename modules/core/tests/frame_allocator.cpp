#include <doctest/doctest.h>
#include <weave/core.hpp>
#include <span>
#include <array>
#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#define WEAVE_TEST_NOINLINE __declspec(noinline)
#else
#define WEAVE_TEST_NOINLINE __attribute__((noinline))
#endif

namespace test_frame_allocator {

using weave::detail::FrameCache;

template <class T>
static weave::Result<T> complete(weave::Task<T> task)
{
  weave::detail::TaskAccess::start(task);
  REQUIRE(weave::detail::TaskAccess::done(task));
  return weave::detail::TaskAccess::take(task);
}

TEST_CASE("Frame cache reuses each size class without aliasing live blocks")
{
  FrameCache cache;
  const std::array<std::size_t, 16>
    sizes{1, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025, 1216, 2047, 2048};
  for (auto size : sizes) {
    CAPTURE(size);
    auto *a = static_cast<std::byte *>(cache.allocate(size));
    auto *b = static_cast<std::byte *>(cache.allocate(size));
    REQUIRE(a != b);
    if (a == b) {
      cache.deallocate(a, size);
      return;
    }
    CHECK(reinterpret_cast<std::uintptr_t>(a) % __STDCPP_DEFAULT_NEW_ALIGNMENT__ == 0);
    std::fill_n(a, size, std::byte{0x25});
    std::fill_n(b, size, std::byte{0x63});
    cache.deallocate(a, size);
    auto *reused = static_cast<std::byte *>(cache.allocate(size));
    CHECK(reused == a);
    std::fill_n(reused, size, std::byte{0x17});
    CHECK(std::all_of(b, b + size, [](auto value) {
      return value == std::byte{0x63};
    }));
    cache.deallocate(reused, size);
    cache.deallocate(b, size);
  }
  cache.clear();
  CHECK(cache.cached_frames() == 0);
}

TEST_CASE("Frame cache has a bounded capacity and bypasses oversized or over-aligned blocks")
{
  FrameCache cache;
  const std::array<std::size_t, 5> capacities{128, 256, 512, 1024, 2048};
  for (auto size : capacities) {
    std::vector<void *> live;
    for (std::size_t i = 0; i < FrameCache::bucket_limit + 10; ++i)
      live.push_back(cache.allocate(size));
    for (auto *memory : live)
      cache.deallocate(memory, size);
  }
  CHECK(cache.cached_frames() == capacities.size() * FrameCache::bucket_limit);
  cache.clear();
  const std::array<std::size_t, 2> oversized{2049, 16384};
  for (auto size : oversized) {
    auto *memory = static_cast<std::byte *>(cache.allocate(size));
    std::fill_n(memory, size, std::byte{0x32});
    cache.deallocate(memory, size);
    CHECK(cache.cached_frames() == 0);
  }
  auto *aligned = cache.allocate(192, 64);
  CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0);
  cache.deallocate(aligned, 192, 64);
  CHECK(cache.cached_frames() == 0);
}

WEAVE_TEST_NOINLINE static weave::Task<std::size_t> checksum(std::span<std::byte> bytes)
{
  std::size_t sum = 0;
  for (auto value : bytes)
    sum += std::to_integer<unsigned>(value);
  co_return sum;
}

static weave::Task<std::size_t> large_frame(std::byte value)
{
  std::array<std::byte, 8192> bytes;
  bytes.fill(value);
  co_return co_await checksum(bytes);
}

struct alignas(64) AlignedValue {
  weave::u64 value;
  std::array<weave::u64, 7> padding{};

  explicit AlignedValue(weave::u64 input) : value(input)
  {
    CHECK(reinterpret_cast<std::uintptr_t>(this) % alignof(AlignedValue) == 0);
  }

  AlignedValue(AlignedValue &&other) noexcept : AlignedValue(other.value)
  {
  }
};

WEAVE_TEST_NOINLINE static weave::Task<AlignedValue> aligned_frame(weave::u64 value)
{
  co_return AlignedValue{value};
}

struct Tracked {
  std::atomic<int> &destroyed;

  ~Tracked()
  {
    ++destroyed;
  }
};

WEAVE_TEST_NOINLINE static weave::Task<int> tracked_frame(std::unique_ptr<Tracked> owned, int &calls)
{
  ++calls;
  co_return owned ? 42 : 0;
}

#if WEAVE_RECYCLE_FRAMES
struct CacheState {
  bool stopped = false;
  bool registered = false;
  std::size_t cached = 0;

  void observe() noexcept
  {
    stopped = weave::detail::frame_cache_stopped;
    registered = weave::detail::frame_cache_registered;
    cached = weave::detail::frame_cache.cached_frames();
  }
};

struct CacheTeardownObserver {
  CacheState &state;

  ~CacheTeardownObserver()
  {
    state.observe();
  }
};

struct TaskAfterCacheCleanup {
  std::optional<weave::Task<int>> &task;
  std::atomic<int> &destroyed;
  int &calls;
  CacheState &state;

  ~TaskAfterCacheCleanup()
  {
    state.observe();
    task.emplace(tracked_frame(std::unique_ptr<Tracked>(new Tracked{destroyed}), calls));
  }
};

struct FrameBeforeCacheCleanup {
  void *memory;
  CacheState &state;

  ~FrameBeforeCacheCleanup()
  {
    state.observe();
    weave::detail::deallocate_frame(memory, 129, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    state.cached = weave::detail::frame_cache.cached_frames();
  }
};

TEST_CASE("A thread's first frame deallocation registers cleanup and reuses foreign storage")
{
  auto *memory = weave::detail::allocate_frame(129, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  CacheState initial;
  CacheState returned;
  CacheState cleaned;
  bool reused = false;
  std::thread destroyer([&] {
    thread_local CacheTeardownObserver observer{cleaned};
    (void)observer;
    initial.observe();
    weave::detail::deallocate_frame(memory, 129, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    returned.observe();
    auto *next = static_cast<std::byte *>(weave::detail::allocate_frame(256, __STDCPP_DEFAULT_NEW_ALIGNMENT__));
    reused = next == memory;
    std::fill_n(next, 256, std::byte{0x36});
    weave::detail::deallocate_frame(next, 256, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  });
  destroyer.join();
  CHECK_FALSE(initial.stopped);
  CHECK_FALSE(initial.registered);
  CHECK(initial.cached == 0);
  CHECK_FALSE(returned.stopped);
  CHECK(returned.registered);
  CHECK(returned.cached == 1);
  CHECK(reused);
  CHECK(cleaned.stopped);
  CHECK(cleaned.registered);
  CHECK(cleaned.cached == 0);
}

TEST_CASE("An earlier TLS destructor creates a lazy task after frame cache cleanup")
{
  std::atomic<int> destroyed = 0;
  int calls = 0;
  std::optional<weave::Task<int>> task;
  CacheState cleaned;
  std::thread creator([&] {
    // Register first so this destructor runs after the cache's cleanup guard.
    thread_local TaskAfterCacheCleanup producer{task, destroyed, calls, cleaned};
    (void)producer;
    auto *memory = weave::detail::allocate_frame(129, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    weave::detail::deallocate_frame(memory, 129, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  });
  creator.join();
  CHECK(cleaned.stopped);
  CHECK(cleaned.registered);
  CHECK(cleaned.cached == 0);
  REQUIRE(task.has_value());
  if (!task)
    return;
  CHECK(calls == 0);
  CHECK(destroyed == 0);
  CHECK(complete(std::move(*task)) == 42);
  task.reset();
  CHECK(calls == 1);
  CHECK(destroyed == 1);
}

TEST_CASE("A later TLS destructor returns a live frame before frame cache cleanup")
{
  CacheState returned;
  CacheState cleaned;
  std::thread owner([&] {
    thread_local CacheTeardownObserver observer{cleaned};
    (void)observer;
    auto *memory = weave::detail::allocate_frame(129, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    // Register last so the retained frame returns while the cache is still live.
    thread_local FrameBeforeCacheCleanup releaser{memory, returned};
    (void)releaser;
  });
  owner.join();
  CHECK_FALSE(returned.stopped);
  CHECK(returned.registered);
  CHECK(returned.cached == 1);
  CHECK(cleaned.stopped);
  CHECK(cleaned.registered);
  CHECK(cleaned.cached == 0);
}
#endif

TEST_CASE("Coroutine frames can outlive their creation thread and destroy parameters exactly once")
{
  for (bool teardown : {false, true}) {
    std::atomic<int> destroyed = 0;
    int calls = 0;
    std::optional<weave::Task<int>> task;
    std::thread creator([&] {
      if (teardown)
        weave::detail::frame_cache_stopped = true;
      task.emplace(tracked_frame(std::unique_ptr<Tracked>(new Tracked{destroyed}), calls));
    });
    creator.join();
    CHECK(calls == 0);
    CHECK(complete(std::move(*task)) == 42);
    task.reset();
    CHECK(calls == 1);
    CHECK(destroyed == 1);
    auto unused = tracked_frame(std::unique_ptr<Tracked>(new Tracked{destroyed}), calls);
    std::thread destroyer([task = std::move(unused)] {
    });
    destroyer.join();
    CHECK(calls == 1);
    CHECK(destroyed == 2);
    CHECK(complete(large_frame(std::byte{3})) == 8192 * 3);
    auto aligned = complete(aligned_frame(17));
    REQUIRE(aligned);
    if (!aligned)
      return;
    CHECK(aligned->value == 17);
  }
}

TEST_CASE("Frames created during cache teardown remain reusable on another thread")
{
  void *memory = nullptr;
  std::thread creator([&] {
    weave::detail::frame_cache_stopped = true;
    memory = weave::detail::allocate_frame(129, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  });
  creator.join();
  weave::detail::deallocate_frame(memory, 129, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
  auto *reused = static_cast<std::byte *>(weave::detail::allocate_frame(256, __STDCPP_DEFAULT_NEW_ALIGNMENT__));
#if WEAVE_RECYCLE_FRAMES
  CHECK(reused == memory);
#endif
  std::fill_n(reused, 256, std::byte{0x36});
  weave::detail::deallocate_frame(reused, 256, __STDCPP_DEFAULT_NEW_ALIGNMENT__);
}

#if defined(WEAVE_DETAIL_ASAN)
TEST_CASE("Cached frame storage is poisoned when dead and unpoisoned on reuse")
{
  FrameCache cache;
  const std::array sizes{
    std::pair{std::size_t{129}, std::size_t{256}},
    std::pair{std::size_t{1025}, std::size_t{2048}}};
  for (const auto [size, capacity] : sizes) {
    CAPTURE(size);
    auto *memory = static_cast<std::byte *>(cache.allocate(size));
    CHECK(__asan_address_is_poisoned(memory) == 0);
    CHECK(__asan_address_is_poisoned(memory + size - 1) == 0);
    CHECK(__asan_address_is_poisoned(memory + size) != 0);
    CHECK(__asan_address_is_poisoned(memory + capacity - 1) != 0);
    std::fill_n(memory, size, std::byte{0x25});
    cache.deallocate(memory, size);
    CHECK(__asan_address_is_poisoned(memory) != 0);
    CHECK(__asan_address_is_poisoned(memory + size - 1) != 0);
    CHECK(__asan_address_is_poisoned(memory + capacity - 1) != 0);
    auto *reused = static_cast<std::byte *>(cache.allocate(capacity));
    REQUIRE(reused == memory);
    if (reused != memory) {
      cache.deallocate(reused, capacity);
      return;
    }
    CHECK(__asan_address_is_poisoned(reused) == 0);
    CHECK(__asan_address_is_poisoned(reused + capacity - 1) == 0);
    std::fill_n(reused, capacity, std::byte{0x41});
    cache.deallocate(reused, capacity);
  }
}
#endif

} // namespace test_frame_allocator
