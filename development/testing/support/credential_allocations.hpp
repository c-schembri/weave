#pragma once

#include <string_view>

namespace fixture {

// Find an addressable live copy before starting its owning Task. The caller must retain that owner.
std::string_view credential_copy(std::string_view pattern) noexcept;

// Used only by isolated executables linked with credential_allocations.cpp.
// The replacement delete observes live storage before freeing it.
struct CredentialAllocation {
  const void *address;
  std::size_t size;
  bool released = false;
  bool zeroed = false;

  explicit CredentialAllocation(std::string_view secret) noexcept;
  CredentialAllocation(const CredentialAllocation &) = delete;
  ~CredentialAllocation();

  bool cleansed() const noexcept;
};

struct CredentialPattern {
  std::string_view pattern;
  std::size_t releases = 0;

  explicit CredentialPattern(std::string_view pattern) noexcept;
  CredentialPattern(const CredentialPattern &) = delete;
  ~CredentialPattern();

  std::size_t dirty_releases() const noexcept;
};

} // namespace fixture
