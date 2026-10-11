#pragma once

#include <weave/database/pool.hpp>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace weave::db::detail {

// Entry destruction and session policy belong to the driver, outside this lock.
template <class Entry>
class PoolInventory {
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<Entry>> idle_;
  PoolStats counters_;

public:
  explicit PoolInventory(std::size_t capacity)
  {
    idle_.reserve(capacity);
  }

  bool begin_acquire() noexcept
  {
    std::lock_guard lock{mutex_};
    if (counters_.closed)
      return false;
    ++counters_.pending;
    return true;
  }

  void finish_acquire(bool discarded) noexcept
  {
    std::lock_guard lock{mutex_};
    --counters_.pending;
    if (discarded)
      ++counters_.discarded;
  }

  bool take_idle(std::unique_ptr<Entry> &entry) noexcept
  {
    std::lock_guard lock{mutex_};
    if (counters_.closed)
      return false;
    if (!idle_.empty()) {
      entry = std::move(idle_.back());
      idle_.pop_back();
    }
    return true;
  }

  bool publish_borrow() noexcept
  {
    std::lock_guard lock{mutex_};
    if (counters_.closed)
      return false;
    ++counters_.borrowed;
    return true;
  }

  void add_idle(std::unique_ptr<Entry> entry)
  {
    std::lock_guard lock{mutex_};
    idle_.push_back(std::move(entry));
    ++counters_.created;
  }

  void created() noexcept
  {
    std::lock_guard lock{mutex_};
    ++counters_.created;
  }

  void discarded() noexcept
  {
    std::lock_guard lock{mutex_};
    ++counters_.discarded;
  }

  // Only bounded, non-reentrant metadata updates belong under the inventory lock.
  template <class Prepare>
    requires std::is_nothrow_invocable_v<Prepare &, Entry &>
  void return_entry(std::unique_ptr<Entry> &entry, bool reusable, Prepare &&prepare) noexcept
  {
    std::lock_guard lock{mutex_};
    --counters_.borrowed;
    if (reusable && !counters_.closed) {
      prepare(*entry);
      idle_.push_back(std::move(entry));
    } else {
      ++counters_.discarded;
    }
  }

  std::vector<std::unique_ptr<Entry>> retire_idle() noexcept
  {
    std::vector<std::unique_ptr<Entry>> retired;
    std::lock_guard lock{mutex_};
    counters_.closed = true;
    retired.swap(idle_);
    counters_.discarded += retired.size();
    return retired;
  }

  PoolStats stats() const noexcept
  {
    std::lock_guard lock{mutex_};
    auto stats = counters_;
    stats.idle = idle_.size();
    return stats;
  }
};

} // namespace weave::db::detail
