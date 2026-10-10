#include <weave/postgres.hpp>
#include "result_access.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#if defined(_MSC_VER)
#include <crtdbg.h>
#endif

namespace storage = weave::pg::detail;

struct Heap {
  std::size_t calls = 0;
  std::size_t live = 0;
  std::size_t limit = std::numeric_limits<std::size_t>::max();

  static void *allocate(void *state, std::size_t size, std::size_t alignment) noexcept
  {
    auto &heap = *static_cast<Heap *>(state);
    ++heap.calls;
    std::printf("request %zu\n", heap.calls);
    std::fflush(stdout);
    if (heap.calls > heap.limit)
      return nullptr;
    auto *data = ::operator new(size, std::align_val_t{alignment}, std::nothrow);
    if (data)
      ++heap.live;
    return data;
  }

  static void deallocate(void *state, void *data, std::size_t, std::size_t alignment) noexcept
  {
    auto &heap = *static_cast<Heap *>(state);
    --heap.live;
    ::operator delete(data, std::align_val_t{alignment});
  }

  storage::ResultHeap provider()
  {
    return {this, allocate, deallocate};
  }
};

int main(int argc, char **argv)
{
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  if (argc != 2)
    return 1;
  const std::string_view scenario = argv[1];
  std::printf("guard %s reached\n", argv[1]);
  std::fflush(stdout);
  Heap heap;

  if (scenario == "pool-failure") {
    heap.limit = 0;
    auto arena = storage::ResultArena::create(heap.provider());
  } else if (scenario == "block-failure") {
    heap.limit = 1;
    auto arena = storage::ResultArena::create(heap.provider());
    arena.allocate(16, 8);
  } else if (scenario == "copy-failure") {
    auto result = storage::ResultAccess::create(heap.provider());
    result.command.assign(80, 'x');
    heap.limit = heap.calls;
    std::printf("armed %zu\n", heap.calls);
    std::fflush(stdout);
    auto copied = result.copy();
  } else if (scenario == "missing-allocate") {
    auto provider = heap.provider();
    provider.allocate = nullptr;
    auto arena = storage::ResultArena::create(provider);
  } else if (scenario == "missing-deallocate") {
    auto provider = heap.provider();
    provider.deallocate = nullptr;
    auto arena = storage::ResultArena::create(provider);
  } else if (scenario == "count-overflow") {
    auto arena = storage::ResultArena::create(heap.provider());
    storage::ResultAllocator<weave::u64> allocator{std::move(arena)};
    allocator.allocate(std::numeric_limits<std::size_t>::max());
  } else if (scenario == "size-overflow") {
    auto arena = storage::ResultArena::create(heap.provider());
    arena.allocate(std::numeric_limits<std::size_t>::max(), 8);
  } else if (scenario == "invalid-alignment" || scenario == "zero-alignment") {
    auto arena = storage::ResultArena::create(heap.provider());
    arena.allocate(16, scenario == "zero-alignment" ? 0 : 3);
  } else if (scenario == "double-free") {
    auto arena = storage::ResultArena::create(heap.provider());
    auto *data = arena.allocate(16, 8);
    storage::ResultStorage::deallocate(data);
    storage::ResultStorage::deallocate(data);
  } else if (scenario == "valid") {
    {
      auto arena = storage::ResultArena::create(heap.provider());
      auto *data = arena.allocate(511, 256);
      if (reinterpret_cast<std::uintptr_t>(data) % 256 != 0)
        return 1;
      storage::ResultStorage::deallocate(data);
      auto *empty = arena.allocate(0, 8);
      storage::ResultStorage::deallocate(empty);
    }
    if (heap.live != 0 || heap.calls != 3)
      return 1;
    std::puts("guard valid passed");
    return 0;
  } else {
    return 1;
  }
  std::puts("guard incorrectly returned");
  return 1;
}
