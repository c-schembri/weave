#pragma once

#include <weave/types.hpp>
#include <chrono>
#include <cstddef>

namespace weave::db {

/// Driver-neutral pool limits, copied by driver factories. Zero idle/lifetime limits disable expiry.
/// Connection creation, validation and reset semantics remain driver-specific.
struct PoolOptions {
  std::size_t max_connections = 16;
  std::size_t min_connections = 1;
  std::chrono::milliseconds acquire_timeout{30000};
  std::chrono::milliseconds idle_timeout{300000};
  std::chrono::milliseconds max_lifetime{1800000};
};

/// Value snapshot of live pool occupancy and cumulative creation/discard counts.
struct PoolStats {
  std::size_t idle = 0;
  std::size_t borrowed = 0;
  std::size_t pending = 0;
  u64 created = 0;
  u64 discarded = 0;
  bool closed = false;
};

/// Check capacity (1..65536, min <= max) and timeout bounds without allocation or side effects.
/// Acquire timeout must be positive; idle/lifetime timeouts may be zero but never negative.
constexpr bool valid_pool_options(const PoolOptions &limits) noexcept
{
  const bool valid_capacity = limits.max_connections > 0 && limits.max_connections <= 65536 &&
    limits.min_connections <= limits.max_connections;
  const bool valid_timeouts = limits.acquire_timeout > std::chrono::milliseconds{0} &&
    limits.idle_timeout >= std::chrono::milliseconds{0} && limits.max_lifetime >= std::chrono::milliseconds{0};
  return valid_capacity && valid_timeouts;
}

} // namespace weave::db
