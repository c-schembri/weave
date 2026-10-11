#pragma once

#include <weave/postgres/connection.hpp>

namespace weave::pg {

struct PoolOptions {
  std::size_t max_connections = 16;
  std::size_t min_connections = 1;
  std::chrono::milliseconds acquire_timeout{30000};
  std::chrono::milliseconds idle_timeout{300000};
  std::chrono::milliseconds max_lifetime{1800000};
};

struct PoolStats {
  std::size_t idle = 0;
  std::size_t borrowed = 0;
  std::size_t pending = 0;
  u64 created = 0;
  u64 discarded = 0;
  bool closed = false;
};

class PooledConnection;

class Pool {
  struct Impl;
  std::shared_ptr<Impl> impl_;
  friend class PooledConnection;

  explicit Pool(std::shared_ptr<Impl> impl) noexcept;
  static Task<Pool> create_impl(detail::OwnedOptions options, PoolOptions limits);
  static Task<PooledConnection> acquire_impl(std::shared_ptr<Impl> impl);
  static Task<PooledConnection> acquire_timed(std::shared_ptr<Impl> impl);

public:
  // Creates initial sessions on the executing Context or work-stealing domain.
  static Task<Pool> create(Options options, PoolOptions limits = {});
  Pool(Pool &&other) noexcept;
  Pool(const Pool &) = delete;
  ~Pool();

  Task<PooledConnection> acquire();
  // Reject/wake acquisitions and close idle sessions. Borrowers finish independently.
  void close() noexcept;
  PoolStats stats() const noexcept;
};

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

  // References and session-specific handles are valid only during this checkout.
  // Drain all method Tasks before releasing or destroying the lease.
  Connection &operator*() const noexcept;
  Connection *operator->() const noexcept;
  // Automatic return also happens at destruction. Both methods empty the lease.
  void release() noexcept;
  void discard() noexcept;
};

} // namespace weave::pg
