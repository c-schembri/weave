#include <weave/mysql/pool.hpp>
#include <weave/semaphore.hpp>
#include <weave/timer.hpp>
#include <weave/io/detail/context_access.hpp>
#include <weave/database/detail/pool_admission.hpp>
#include "pool_access.hpp"

namespace weave::mysql {

struct PooledConnection::Entry {
  Connection connection;
  std::optional<Semaphore::Permit> permit;
  std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point returned = created;
  bool dirty = false;

  explicit Entry(Connection session) : connection(std::move(session))
  {
    detail::PoolAccess::enroll(connection);
  }
};

struct Pool::Impl : db::detail::PoolInventory<PooledConnection::Entry> {
  Options options;
  PoolOptions limits;
  Context &context;
  Semaphore capacity;

  Impl(Options configuration, PoolOptions policy, Context &owner)
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

Pool::Pool(Pool &&) noexcept = default;

Pool::~Pool()
{
  close();
}

Task<Pool> Pool::create(Options options, PoolOptions limits)
{
  return create_impl(std::move(options), limits);
}

Task<Pool> Pool::create_impl(Options options, PoolOptions limits)
{
  if (!db::valid_pool_options(limits))
    co_await fail(std::errc::invalid_argument);
  weave::detail::require(weave::detail::current_context != nullptr);
  co_await cancellation_point();
  auto impl = std::make_shared<Impl>(std::move(options), limits, *weave::detail::current_context);
  Pool pool{impl};
  for (std::size_t index = 0; index < limits.min_connections; ++index) {
    auto connection = co_await connect(impl->options);
    impl->add_idle(std::make_unique<PooledConnection::Entry>(std::move(connection)));
  }
  co_return pool;
}

Task<PooledConnection> Pool::acquire()
{
  weave::detail::require(impl_ != nullptr);
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
    co_await fail(make_error_code(ClientError::closed));
  db::detail::PoolAdmission<PooledConnection::Entry> admission{*impl};
  weave::detail::ContextAccess::check_execution(impl->context);
  auto permit = co_await as_result(impl->capacity.acquire());
  if (!permit) {
    if (permit.error() == make_error_code(SyncError::closed))
      co_await fail(make_error_code(ClientError::closed));
    co_await fail(permit.error());
  }
  admission.permit.emplace(std::move(*permit));
  if (!impl->take_idle(admission.entry))
    co_await fail(make_error_code(ClientError::closed));
  auto &entry = admission.entry;
  if (entry) {
    const auto now = std::chrono::steady_clock::now();
    const bool idle = impl->limits.idle_timeout.count() && now - entry->returned >= impl->limits.idle_timeout;
    const bool expired = impl->limits.max_lifetime.count() && now - entry->created >= impl->limits.max_lifetime;
    if (idle || expired || !entry->connection.open()) {
      entry.reset();
      impl->discarded();
    }
  }
  if (entry && entry->dirty) {
    co_await entry->connection.reset();
    if (impl->options.database.empty()) {
      auto result = co_await entry->connection.execute("SELECT DATABASE() AS current_database");
      if (result.rows.size() != 1)
        co_await fail(make_error_code(ClientError::protocol));
      auto selected = result.rows[0]["current_database"].as<std::optional<std::string_view>>();
      if (!selected)
        co_await fail(selected.error());
      // COM_RESET_CONNECTION cannot deselect a database chosen by USE. Reconnect to restore absence.
      if (*selected) {
        entry.reset();
        impl->discarded();
      }
    }
    if (entry)
      entry->dirty = false;
  }
  if (!entry) {
    auto connection = co_await connect(impl->options);
    entry = std::make_unique<PooledConnection::Entry>(std::move(connection));
    impl->created();
  }
  co_await cancellation_point();
  if (!impl->publish_borrow())
    co_await fail(make_error_code(ClientError::closed));
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

PooledConnection::PooledConnection(PooledConnection &&) noexcept = default;

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
  const auto reusable = detail::PoolAccess::recycle(entry->connection);
  auto permit = std::move(entry->permit);
  entry->permit.reset();
  pool->return_entry(entry, !discard && reusable, [](Entry &returned) noexcept {
    returned.dirty = true;
    returned.returned = std::chrono::steady_clock::now();
  });
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

} // namespace weave::mysql
