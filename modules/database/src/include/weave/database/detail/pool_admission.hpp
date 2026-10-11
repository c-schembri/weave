#pragma once

#include <weave/database/detail/pool_inventory.hpp>
#include <weave/semaphore.hpp>
#include <optional>

namespace weave::db::detail {

template <class Entry>
struct PoolAdmission {
  PoolInventory<Entry> &pool;
  std::unique_ptr<Entry> entry;
  std::optional<Semaphore::Permit> permit;

  // Async drivers construct this after begin_acquire; inventory and semaphore outlive the guard.
  explicit PoolAdmission(PoolInventory<Entry> &inventory) noexcept : pool(inventory)
  {
  }

  PoolAdmission(const PoolAdmission &) = delete;

  ~PoolAdmission()
  {
    pool.finish_acquire(entry != nullptr);
    // Retire the session before refunding capacity, including failure/cancellation.
    entry.reset();
    permit.reset();
  }
};

} // namespace weave::db::detail
