#include <doctest/doctest.h>
#include <weave/core.hpp>
#include <span>
#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <thread>
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
  for (std::size_t size : {1, 127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024}) {
    CAPTURE(size);
    auto *a = static_cast<std::byte *>(cache.allocate(size));
    auto *b = static_cast<std::byte *>(cache.allocate(size));
    REQUIRE(a != b);
    CHECK(reinterpret_cast<std::uintptr_t>(a) % __STDCPP_DEFAULT_NEW_ALIGNMENT__ == 0);
    std::fill_n(a, size, std::byte{0x25});
    std::fill_n(b, size, std::byte{0x63});
    cache.deallocate(a, size);
    auto *reused = static_cast<std::byte *>(cache.allocate(size));
    CHECK(reused == a);
    std::fill_n(reused, size, std::byte{0x17});
    CHECK(std::all_of(b, b + size, [](auto value) { return value == std::byte{0x63}; }));
    cache.deallocate(reused, size);
    cache.deallocate(b, size);
  }
  cache.clear();
  CHECK(cache.cached_frames() == 0);
}

TEST_CASE("Frame cache has a bounded capacity and bypasses oversized or over-aligned blocks")
{
  FrameCache cache;
  for (std::size_t size : {128, 256, 512, 1024}) {
    std::vector<void *> live;
    for (std::size_t i = 0; i < FrameCache::bucket_limit + 10; ++i)
      live.push_back(cache.allocate(size));
    for (auto *memory : live)
      cache.deallocate(memory, size);
  }
  CHECK(cache.cached_frames() == 4 * FrameCache::bucket_limit);
  cache.clear();
  for (std::size_t size : {1025, 16384}) {
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
    std::thread destroyer([task = std::move(unused)] {});
    destroyer.join();
    CHECK(calls == 1);
    CHECK(destroyed == 2);
    CHECK(complete(large_frame(std::byte{3})) == 8192 * 3);
    auto aligned = complete(aligned_frame(17));
    REQUIRE(aligned);
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
  auto *memory = static_cast<std::byte *>(cache.allocate(129));
  CHECK(__asan_address_is_poisoned(memory) == 0);
  CHECK(__asan_address_is_poisoned(memory + 128) == 0);
  CHECK(__asan_address_is_poisoned(memory + 129) != 0);
  cache.deallocate(memory, 129);
  CHECK(__asan_address_is_poisoned(memory) != 0);
  CHECK(__asan_address_is_poisoned(memory + 128) != 0);
  auto *reused = static_cast<std::byte *>(cache.allocate(256));
  REQUIRE(reused == memory);
  CHECK(__asan_address_is_poisoned(reused + 255) == 0);
  std::fill_n(reused, 256, std::byte{0x41});
  cache.deallocate(reused, 256);
}
#endif

} // namespace test_frame_allocator
