#pragma once

#include <weave/postgres/connection.hpp>
#include <weave/database/pool.hpp>

namespace weave::pg {

/// Shared pool sizing/expiry policy used by the PostgreSQL driver.
using PoolOptions = db::PoolOptions;
/// Snapshot of PostgreSQL pool occupancy and cumulative session turnover.
using PoolStats = db::PoolStats;

class PooledConnection;

/// Move-only asynchronous session pool bound to its creating Context or work-stealing domain.
/// Sessions are exclusively checked out; release schedules reset before reuse, not immediate availability.
class Pool {
  struct Impl;
  std::shared_ptr<Impl> impl_;
  friend class PooledConnection;

  explicit Pool(std::shared_ptr<Impl> impl) noexcept;
  static Task<Pool> create_impl(detail::OwnedOptions options, PoolOptions limits);
  static Task<PooledConnection> acquire_impl(std::shared_ptr<Impl> impl);
  static Task<PooledConnection> acquire_timed(std::shared_ptr<Impl> impl);

public:
  /// Create initial sessions on the executing Context or work-stealing domain.
  /// Capture owned configuration before the lazy Task starts: create(options) copies,
  /// create(std::move(options)) transfers. The source Options need not outlive the Task or pool.
  /// The pool retains configuration to open replacement sessions; callback-captured references remain caller-owned.
  static Task<Pool> create(Options options, PoolOptions limits = {});
  Pool(Pool &&other) noexcept;
  Pool(const Pool &) = delete;
  ~Pool();

  /// Wait for an owned checkout, retaining the pool's shared state in the lazy Task.
  /// Closing the pool rejects acquisitions; a successful lease keeps its session alive until returned/discarded.
  Task<PooledConnection> acquire();
  /// Reject/wake acquisitions and close idle sessions; borrowers finish independently. Does not join.
  void close() noexcept;
  /// Synchronous thread-safe occupancy snapshot; not a readiness guarantee for the next acquire.
  PoolStats stats() const noexcept;
};

/// Move-only exclusive session checkout. Destruction returns it; active operations must drain first.
class [[nodiscard]] PooledConnection {
  struct Entry;
  std::unique_ptr<Entry> entry_;
  std::shared_ptr<Pool::Impl> pool_;
  friend class Pool;

  PooledConnection(std::unique_ptr<Entry> entry, std::shared_ptr<Pool::Impl> pool) noexcept;
  void return_connection(bool discard) noexcept;

public:
  PooledConnection(PooledConnection &&other) noexcept;
  PooledConnection &operator=(PooledConnection &&other) noexcept;
  PooledConnection(const PooledConnection &) = delete;
  ~PooledConnection();

  /// Borrow the checked-out session. References and session-specific handles are valid only during this checkout.
  /// Drain all method Tasks before releasing, discarding or destroying the lease.
  Connection &operator*() const noexcept;
  /// Borrow the session with the same checkout lifetime as operator*(); this does not transfer ownership.
  Connection *operator->() const noexcept;
  /// Return the session for pool-managed reset/reuse; destruction also returns it automatically.
  /// Empties this lease. All session operations and borrowed references must be finished first.
  void release() noexcept;
  /// Retire the session instead of reusing it. Empties this lease; drain session operations first.
  void discard() noexcept;
};

} // namespace weave::pg
