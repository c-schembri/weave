#include "credential_allocations.hpp"
#include <array>
#include <cstdlib>
#include <mutex>
#include <new>
#include <cstring>
#include <cstdio>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FIXTURE_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(FIXTURE_ASAN)
#define FIXTURE_ASAN 1
#endif
#if defined(FIXTURE_ASAN)
#include <sanitizer/asan_interface.h>
#endif

#ifndef WEAVE_CREDENTIAL_ALLOCATION_SLOTS
#define WEAVE_CREDENTIAL_ALLOCATION_SLOTS 8192
#endif

namespace {

std::mutex mutex;
std::array<fixture::CredentialAllocation *, 256> watched{};
std::array<fixture::CredentialPattern *, 32> patterns{};

struct Allocation {
  void *pointer = nullptr;
  std::size_t size = 0;
};

std::array<Allocation, WEAVE_CREDENTIAL_ALLOCATION_SLOTS> allocations{};

void *allocate(std::size_t size) noexcept
{
  // Initialize fixture storage so pattern scans never read indeterminate bytes.
  auto *pointer = std::calloc(size ? size : 1, 1);
  if (!pointer)
    return nullptr;

  std::lock_guard lock{mutex};
  for (auto &slot : allocations) {
    if (!slot.pointer) {
      slot = {pointer, size};
      return pointer;
    }
  }
  std::fprintf(stderr, "Allocation witness table exhausted: %zu slots\n", allocations.size());
  std::fflush(stderr);
  std::abort();
}

void release(void *pointer) noexcept
{
  {
    std::lock_guard lock{mutex};
    for (auto &slot : allocations) {
      if (slot.pointer != pointer || !pointer)
        continue;

      const auto *bytes = static_cast<const char *>(pointer);
      for (auto *observer : patterns) {
        if (!observer)
          continue;
        auto size = observer->pattern.size();
        for (std::size_t index = 0; index + size <= slot.size; ++index) {
          if (std::memcmp(bytes + index, observer->pattern.data(), size) == 0) {
            ++observer->releases;
            break;
          }
        }
      }
      slot = {};
      break;
    }
    for (auto *allocation : watched) {
      if (!allocation || allocation->address != pointer || !pointer)
        continue;

      allocation->released = true;
      allocation->zeroed = true;
      auto *bytes = static_cast<const unsigned char *>(pointer);
      for (std::size_t index = 0; index < allocation->size; ++index)
        allocation->zeroed &= bytes[index] == 0;

      allocation->address = nullptr;
    }
  }
  std::free(pointer);
}

} // namespace

std::string_view fixture::credential_copy(std::string_view pattern) noexcept
{
  std::lock_guard lock{mutex};
  for (const auto &slot : allocations) {
    if (!slot.pointer || slot.pointer == pattern.data() || slot.size < pattern.size())
      continue;
#if defined(FIXTURE_ASAN)
    // Cached dead frames remain allocated but poisoned; they are not live credential copies.
    if (__asan_region_is_poisoned(slot.pointer, slot.size))
      continue;
#endif
    auto *bytes = static_cast<const char *>(slot.pointer);
    if (std::memcmp(bytes, pattern.data(), pattern.size()) == 0)
      return {bytes, slot.size};
  }
  return {};
}

fixture::CredentialAllocation::CredentialAllocation(std::string_view secret) noexcept
    : address(secret.data()), size(secret.size())
{
  std::lock_guard lock{mutex};
  for (auto &slot : watched) {
    if (!slot) {
      slot = this;
      return;
    }
  }
  std::abort();
}

fixture::CredentialAllocation::~CredentialAllocation()
{
  std::lock_guard lock{mutex};
  for (auto &slot : watched) {
    if (slot == this)
      slot = nullptr;
  }
}

bool fixture::CredentialAllocation::cleansed() const noexcept
{
  std::lock_guard lock{mutex};
  return released && zeroed;
}

fixture::CredentialPattern::CredentialPattern(std::string_view pattern) noexcept : pattern(pattern)
{
  if (pattern.empty())
    std::abort();

  std::lock_guard lock{mutex};
  for (auto &slot : patterns) {
    if (!slot) {
      slot = this;
      return;
    }
  }
  std::abort();
}

fixture::CredentialPattern::~CredentialPattern()
{
  std::lock_guard lock{mutex};
  for (auto &slot : patterns) {
    if (slot == this)
      slot = nullptr;
  }
}

std::size_t fixture::CredentialPattern::dirty_releases() const noexcept
{
  std::lock_guard lock{mutex};
  return releases;
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

void operator delete(void *pointer) noexcept
{
  release(pointer);
}

void operator delete[](void *pointer) noexcept
{
  release(pointer);
}

void operator delete(void *pointer, std::size_t) noexcept
{
  release(pointer);
}

void operator delete[](void *pointer, std::size_t) noexcept
{
  release(pointer);
}

void operator delete(void *pointer, const std::nothrow_t &) noexcept
{
  release(pointer);
}

void operator delete[](void *pointer, const std::nothrow_t &) noexcept
{
  release(pointer);
}
