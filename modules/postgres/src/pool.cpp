#include <weave/postgres/pool.hpp>
#include <weave/semaphore.hpp>
#include <weave/timer.hpp>
#include <weave/io/detail/context_access.hpp>
#include <weave/database/detail/pool_admission.hpp>
#include "options.hpp"
#include "pool_access.hpp"

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

struct Pool::Impl : db::detail::PoolInventory<PooledConnection::Entry> {
  detail::OwnedOptions options;
  PoolOptions limits;
  Context &context;
  Semaphore capacity;

  Impl(detail::OwnedOptions configuration, PoolOptions policy, Context &owner)
      : PoolInventory(policy.max_connections), options(std::move(configuration)), limits(policy), context(owner),
        capacity(policy.max_connections)
  {
  }

  void stop() noexcept
  {
    auto retired = retire_idle();

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
  if (!db::valid_pool_options(limits))
    co_await fail(std::errc::invalid_argument);
  if (options.value.replication != Replication::disabled)
    co_await fail(std::errc::operation_not_supported);

  weave::detail::require(weave::detail::current_context != nullptr);
  co_await cancellation_point();
  auto impl = std::make_shared<Impl>(std::move(options), limits, *weave::detail::current_context);
  Pool pool{impl};

  for (std::size_t index = 0; index < limits.min_connections; ++index) {
    auto session = co_await connect(impl->options.value);
    impl->add_idle(std::make_unique<PooledConnection::Entry>(std::move(session)));
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
  if (!impl->begin_acquire())
    co_await fail(Error::closed);

  db::detail::PoolAdmission<PooledConnection::Entry> admission{*impl};

  weave::detail::ContextAccess::check_execution(impl->context);
  auto admitted = co_await as_result(impl->capacity.acquire());
  if (!admitted) {
    if (admitted.error() == make_error_code(SyncError::closed))
      co_await fail(Error::closed);
    co_await fail(admitted.error());
  }
  admission.permit.emplace(std::move(*admitted));

  if (!impl->take_idle(admission.entry))
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
      impl->discarded();
    }
  }

  if (!entry) {
    auto session = co_await connect(impl->options.value);
    entry = std::make_unique<PooledConnection::Entry>(std::move(session));
    impl->created();
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
  if (!impl->publish_borrow())
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
  return impl_->stats();
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

  pool->return_entry(entry, !discard && reusable, [](Entry &returned) noexcept {
    returned.dirty = true;
    returned.returned = std::chrono::steady_clock::now();
  });

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
