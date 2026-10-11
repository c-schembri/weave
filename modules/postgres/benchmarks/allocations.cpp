#include "allocations.hpp"
#include <cstdlib>
#include <limits>
#include <new>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace {

thread_local bool counting = false;
thread_local bench::Allocations allocations;

void record(std::size_t size) noexcept
{
  if (counting) {
    ++allocations.calls;
    allocations.bytes += size;
  }
}

void *allocate(std::size_t size, std::size_t alignment = 0) noexcept
{
  void *pointer = nullptr;
  auto actual = size == 0 ? 1 : size;
  if (alignment == 0) {
    pointer = std::malloc(actual);
  } else {
#ifdef _WIN32
    pointer = _aligned_malloc(actual, alignment);
#else
    if (actual > std::numeric_limits<std::size_t>::max() - (alignment - 1))
      return nullptr;
    auto rounded = (actual + alignment - 1) / alignment * alignment;
    pointer = std::aligned_alloc(alignment, rounded);
#endif
  }
  if (pointer)
    record(size);
  return pointer;
}

void aligned_free(void *pointer) noexcept
{
#ifdef _WIN32
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}

} // namespace

bench::AllocationSample::AllocationSample(bool enabled) noexcept
{
  allocations = {};
  counting = enabled;
}

bench::AllocationSample::~AllocationSample()
{
  counting = false;
}

bench::Allocations bench::AllocationSample::finish() noexcept
{
  counting = false;
  return allocations;
}

void *operator new(std::size_t size)
{
  auto *pointer = allocate(size);
  if (!pointer)
    std::abort();
  return pointer;
}

void *operator new[](std::size_t size)
{
  return ::operator new(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
  return allocate(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept
{
  return ::operator new(size, tag);
}

void *operator new(std::size_t size, std::align_val_t alignment)
{
  auto *pointer = allocate(size, static_cast<std::size_t>(alignment));
  if (!pointer)
    std::abort();
  return pointer;
}

void *operator new[](std::size_t size, std::align_val_t alignment)
{
  return ::operator new(size, alignment);
}

void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
  return allocate(size, static_cast<std::size_t>(alignment));
}

void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &tag) noexcept
{
  return ::operator new(size, alignment, tag);
}

void operator delete(void *pointer) noexcept
{
  std::free(pointer);
}

void operator delete[](void *pointer) noexcept
{
  std::free(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept
{
  std::free(pointer);
}

void operator delete[](void *pointer, std::size_t) noexcept
{
  std::free(pointer);
}

void operator delete(void *pointer, const std::nothrow_t &) noexcept
{
  std::free(pointer);
}

void operator delete[](void *pointer, const std::nothrow_t &) noexcept
{
  std::free(pointer);
}

void operator delete(void *pointer, std::align_val_t) noexcept
{
  aligned_free(pointer);
}

void operator delete[](void *pointer, std::align_val_t) noexcept
{
  aligned_free(pointer);
}

void operator delete(void *pointer, std::size_t, std::align_val_t) noexcept
{
  aligned_free(pointer);
}

void operator delete[](void *pointer, std::size_t, std::align_val_t) noexcept
{
  aligned_free(pointer);
}

void operator delete(void *pointer, std::align_val_t, const std::nothrow_t &) noexcept
{
  aligned_free(pointer);
}

void operator delete[](void *pointer, std::align_val_t, const std::nothrow_t &) noexcept
{
  aligned_free(pointer);
}
