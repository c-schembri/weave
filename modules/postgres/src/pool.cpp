#include <weave/postgres/pool.hpp>
#include <weave/semaphore.hpp>
#include <weave/timer.hpp>
#include <weave/io/detail/context_access.hpp>
#include "options.hpp"
#include "pool_access.hpp"
#include <mutex>

namespace weave::pg {

struct PooledConnection::Entry {
  Connection connection;
  std::optional<Semaphore::Permit> permit;
  std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point returned = created;
  bool dirty = false;

  explicit Entry(Connection session) noexcept : connection(std::move(session))
  {
    detail::PoolAccess::enroll(connection);
  }
};

struct Pool::Impl {
  detail::OwnedOptions options;
  PoolOptions limits;
  Context &context;
  Semaphore capacity;
  mutable std::mutex mutex;
  std::vector<std::unique_ptr<PooledConnection::Entry>> idle;
  PoolStats counters;

  Impl(detail::OwnedOptions configuration, PoolOptions policy, Context &owner)
      : options(std::move(configuration)), limits(policy), context(owner), capacity(policy.max_connections)
  {
    idle.reserve(limits.max_connections);
  }

  void stop() noexcept
  {
    std::vector<std::unique_ptr<PooledConnection::Entry>> retired;
    {
      std::lock_guard lock{mutex};
      counters.closed = true;
      retired.swap(idle);
      counters.discarded += retired.size();
    }

    capacity.close();
    if (!retired.empty())
      weave::detail::ContextAccess::check_thread(context);
  }
};

Pool::Pool(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

Pool::Pool(Pool &&other) noexcept = default;

Pool::~Pool()
{
  close();
}

Task<Pool> Pool::create(Options options, PoolOptions limits)
{
  return create_impl(detail::OwnedOptions{std::move(options)}, limits);
}

Task<Pool> Pool::create_impl(detail::OwnedOptions options, PoolOptions limits)
{
  const bool invalid_capacity = limits.max_connections == 0 || limits.max_connections > 65536 ||
    limits.min_connections > limits.max_connections;
  const bool invalid_timeouts = limits.acquire_timeout <= std::chrono::milliseconds{0} ||
    limits.idle_timeout < std::chrono::milliseconds{0} || limits.max_lifetime < std::chrono::milliseconds{0};
  if (invalid_capacity || invalid_timeouts)
    co_await fail(std::errc::invalid_argument);
  if (options.value.replication != Replication::disabled)
    co_await fail(std::errc::operation_not_supported);

  weave::detail::require(weave::detail::current_context != nullptr);
  co_await cancellation_point();
  auto impl = std::make_shared<Impl>(std::move(options), limits, *weave::detail::current_context);
  Pool pool{impl};

  for (std::size_t index = 0; index < limits.min_connections; ++index) {
    auto session = co_await connect(impl->options.value);
    impl->idle.push_back(std::make_unique<PooledConnection::Entry>(std::move(session)));
    ++impl->counters.created;
  }

  co_return pool;
}

Task<PooledConnection> Pool::acquire()
{
  weave::detail::require(impl_ != nullptr);
  // Capture state now, including for a deferred Task that outlives the Pool handle.
  return acquire_timed(impl_);
}

Task<PooledConnection> Pool::acquire_timed(std::shared_ptr<Impl> impl)
{
  const auto duration = impl->limits.acquire_timeout;
  co_return co_await timeout(duration, acquire_impl(std::move(impl)));
}

Task<PooledConnection> Pool::acquire_impl(std::shared_ptr<Impl> impl)
{
  bool closed = false;
  {
    std::lock_guard lock{impl->mutex};
    closed = impl->counters.closed;
    if (!closed)
      ++impl->counters.pending;
  }
  if (closed)
    co_await fail(Error::closed);

  struct Admission {
    Impl &pool;
    std::unique_ptr<PooledConnection::Entry> entry;
    std::optional<Semaphore::Permit> permit;

    ~Admission()
    {
      {
        std::lock_guard lock{pool.mutex};
        --pool.counters.pending;
        if (entry)
          ++pool.counters.discarded;
      }

      entry.reset();
      permit.reset();
    }
  } admission{*impl};

  weave::detail::ContextAccess::check_execution(impl->context);
  auto admitted = co_await as_result(impl->capacity.acquire());
  if (!admitted) {
    if (admitted.error() == make_error_code(SyncError::closed))
      co_await fail(Error::closed);
    co_await fail(admitted.error());
  }
  admission.permit.emplace(std::move(*admitted));

  {
    std::lock_guard lock{impl->mutex};
    closed = impl->counters.closed;
    if (!closed && !impl->idle.empty()) {
      admission.entry = std::move(impl->idle.back());
      impl->idle.pop_back();
    }
  }
  if (closed)
    co_await fail(Error::closed);

  auto &entry = admission.entry;
  if (entry) {
    const auto now = std::chrono::steady_clock::now();
    const bool expired_idle = impl->limits.idle_timeout.count() != 0 &&
      now - entry->returned >= impl->limits.idle_timeout;
    const bool expired_session = impl->limits.max_lifetime.count() != 0 &&
      now - entry->created >= impl->limits.max_lifetime;
    if (expired_idle || expired_session || !entry->connection.open()) {
      entry.reset();
      std::lock_guard lock{impl->mutex};
      ++impl->counters.discarded;
    }
  }

  if (!entry) {
    auto session = co_await connect(impl->options.value);
    entry = std::make_unique<PooledConnection::Entry>(std::move(session));
    std::lock_guard lock{impl->mutex};
    ++impl->counters.created;
  } else if (entry->dirty) {
    auto &session = entry->connection;
    const auto transaction = session.transaction();
    if (transaction == Transaction::active || transaction == Transaction::failed)
      co_await session.query("ROLLBACK");

    co_await session.query("DISCARD ALL");
    if (!detail::PoolAccess::recycle(session) || session.transaction() != Transaction::idle)
      co_await fail(Error::protocol);
    entry->dirty = false;
  }

  co_await cancellation_point();
  {
    std::lock_guard lock{impl->mutex};
    closed = impl->counters.closed;
    if (!closed)
      ++impl->counters.borrowed;
  }
  if (closed)
    co_await fail(Error::closed);

  entry->permit.emplace(std::move(*admission.permit));
  co_return PooledConnection{std::move(entry), impl};
}

void Pool::close() noexcept
{
  if (impl_)
    impl_->stop();
}

PoolStats Pool::stats() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  std::lock_guard lock{impl_->mutex};
  auto stats = impl_->counters;
  stats.idle = impl_->idle.size();
  return stats;
}

PooledConnection::PooledConnection(std::unique_ptr<Entry> entry, std::shared_ptr<Pool::Impl> pool) noexcept
    : entry_(std::move(entry)), pool_(std::move(pool))
{
}

PooledConnection::PooledConnection(PooledConnection &&other) noexcept = default;

PooledConnection &PooledConnection::operator=(PooledConnection &&other) noexcept
{
  if (this != &other) {
    release();
    entry_ = std::move(other.entry_);
    pool_ = std::move(other.pool_);
  }

  return *this;
}

PooledConnection::~PooledConnection()
{
  release();
}

Connection &PooledConnection::operator*() const noexcept
{
  weave::detail::require(entry_ != nullptr);
  return entry_->connection;
}

Connection *PooledConnection::operator->() const noexcept
{
  return &**this;
}

void PooledConnection::return_connection(bool discard) noexcept
{
  if (!entry_)
    return;

  auto entry = std::move(entry_);
  auto pool = std::move(pool_);
  weave::detail::ContextAccess::check_thread(pool->context);
  const bool reusable = detail::PoolAccess::recycle(entry->connection);
  auto permit = std::move(entry->permit);
  entry->permit.reset();

  {
    std::lock_guard lock{pool->mutex};
    --pool->counters.borrowed;
    if (!discard && reusable && !pool->counters.closed) {
      entry->dirty = true;
      entry->returned = std::chrono::steady_clock::now();
      pool->idle.push_back(std::move(entry));
    } else {
      ++pool->counters.discarded;
    }
  }

  // Free retired sessions before handing their capacity to another acquirer.
  entry.reset();
  permit.reset();
}

void PooledConnection::release() noexcept
{
  return_connection(false);
}

void PooledConnection::discard() noexcept
{
  return_connection(true);
}

} // namespace weave::pg
