#pragma once

#include <weave/mysql/connection.hpp>
#include <weave/database/pool.hpp>

namespace weave::mysql {

using PoolOptions = db::PoolOptions;
using PoolStats = db::PoolStats;
class PooledConnection;

/// Exclusive session inventory bound to its creating Context/work-stealing domain.
/// Return marks a session dirty; its next borrower waits for reset/validation before reuse.
class Pool {
  struct Impl;
  std::shared_ptr<Impl> impl_;
  friend class PooledConnection;
  explicit Pool(std::shared_ptr<Impl> impl) noexcept;
  static Task<Pool> create_impl(Options options, PoolOptions limits);
  static Task<PooledConnection> acquire_impl(std::shared_ptr<Impl> impl);
  static Task<PooledConnection> acquire_timed(std::shared_ptr<Impl> impl);

public:
  /// Own configuration before suspension (copy lvalues, transfer rvalues); retain it for replacement sessions.
  static Task<Pool> create(Options options, PoolOptions limits = {});
  Pool(Pool &&) noexcept;
  Pool(const Pool &) = delete;
  ~Pool();
  /// Wait for capacity, retaining pool state even if the handle is destroyed before this Task starts.
  Task<PooledConnection> acquire();
  /// Reject/wake acquisitions and retire idle sessions. Borrowers drain independently; this does not join.
  void close() noexcept;
  /// Thread-safe occupancy/turnover snapshot, not an admission guarantee.
  PoolStats stats() const noexcept;
};

/// Move-only exclusive checkout. Destroy/release on compatible execution after all method Tasks,
/// Statement handles and unfinished RowStreams are gone; violating that lifetime contract is fatal.
class [[nodiscard]] PooledConnection {
  struct Entry;
  std::unique_ptr<Entry> entry_;
  std::shared_ptr<Pool::Impl> pool_;
  friend class Pool;
  PooledConnection(std::unique_ptr<Entry> entry, std::shared_ptr<Pool::Impl> pool) noexcept;
  void return_connection(bool discard) noexcept;

public:
  PooledConnection(PooledConnection &&) noexcept;
  PooledConnection &operator=(PooledConnection &&) noexcept;
  PooledConnection(const PooledConnection &) = delete;
  ~PooledConnection();
  /// Borrow the session only for this checkout; neither member transfers ownership.
  Connection &operator*() const noexcept;
  Connection *operator->() const noexcept;
  /// Return for pool-managed reset; empties the lease. Destruction does the same.
  void release() noexcept;
  /// Retire instead of reusing; empties the lease.
  void discard() noexcept;
};

} // namespace weave::mysql
