#include "gss_pool.hpp"

namespace weave::pg {

namespace {

template <class T, class F>
struct ProviderAwaiter : detail::GssWork {
  detail::GssPool &pool;
  F &function;
  bool cleanup;
  weave::detail::Posted posted;
  std::mutex publication;
  std::coroutine_handle<> continuation;
  CancelToken cancellation;
  CancelToken stopping;
  Result<T> result = std::unexpected(std::make_error_code(std::errc::operation_not_permitted));

  ProviderAwaiter(detail::GssPool &owner, F &body, bool shielded) : pool(owner), function(body), cleanup(shielded)
  {
  }

  bool cancelled() const noexcept
  {
    return !cleanup && (cancellation.stop_requested() || stopping.stop_requested());
  }

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> parent)
  {
    auto *context = weave::detail::current_context;
    weave::detail::require(context);
    continuation = parent;
    cancellation = parent.promise().cancellation;
    stopping = weave::detail::context_cancellation(*context);
    if (cancelled()) {
      result = std::unexpected(std::make_error_code(std::errc::operation_canceled));
      return false;
    }

    posted.target = context;
    posted.executor = weave::detail::current_executor;
    posted.state = this;
    posted.invoke = [](void *state) noexcept {
      auto &self = *static_cast<ProviderAwaiter *>(state);
      std::coroutine_handle<> resume;
      {
        // The foreign publisher must leave Context/Executor before the frame can be reclaimed.
        std::lock_guard lock(self.publication);
        resume = self.continuation;
      }
      resume.resume();
    };
    invoke = [](detail::GssWork &work) noexcept {
      auto &self = static_cast<ProviderAwaiter &>(work);
      if (self.cancelled())
        self.result = std::unexpected(std::make_error_code(std::errc::operation_canceled));
      else
        self.result = self.function();

      // Resumption may destroy the callable and frame. Publication is the final access.
      std::lock_guard lock(self.publication);
      weave::detail::post(*self.posted.target, self.posted);
    };
    pool.submit(*this);
    return true;
  }

  Result<T> await_resume() noexcept
  {
    if (cancelled())
      return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    return std::move(result);
  }
};

template <class T, class F>
Task<T> provider_call(detail::GssPool &pool, F function, bool cleanup = false)
{
  ProviderAwaiter<T, F> pending{pool, function, cleanup};
  auto result = co_await pending;
  if (!result)
    co_await fail(result.error());
  if constexpr (!std::is_void_v<T>)
    co_return std::move(*result);
}

struct Pending {
  bool &value;

  explicit Pending(bool &state) : value(state)
  {
    weave::detail::require(!value);
    value = true;
  }

  ~Pending()
  {
    value = false;
  }
};

} // namespace

Result<GssContext> GssContext::create(GssContextOptions options)
{
  if (options.workers == 0 || options.workers > 64 || options.capacity < options.workers || options.capacity > 65536 ||
    options.credential_cache.size() > 65536 || options.credential_cache.find('\0') != std::string::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (!detail::Gss::available())
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  auto pool = std::make_shared<detail::GssPool>(std::move(options));
  if (auto started = pool->start(); !started)
    return std::unexpected(started.error());
  return GssContext{std::move(pool)};
}

void detail::GssPool::stop() noexcept
{
  {
    std::lock_guard lock(mutex_);
    // Last-owner teardown may still have transferred native destructors to the pool.
    weave::detail::require(sessions_ == retired_ && queued_ <= retired_);
    stopping_ = true;
  }
  ready_.notify_all();
}

void detail::GssPool::drained() noexcept
{
  std::lock_guard lock(mutex_);
  weave::detail::require(sessions_ == 0 && retired_ == 0 && queued_ == 0 && !first_ && !last_);
}

Result<void> detail::GssPool::wait_started()
{
  std::unique_lock lock(mutex_);
  ready_.wait(lock, [&] {
    return started_ == options_.workers;
  });
  if (startup_error_)
    return std::unexpected(startup_error_);
  return {};
}

void detail::GssPool::worker(std::error_code startup_error) noexcept
{
  {
    std::lock_guard lock(mutex_);
    if (!startup_error_ && startup_error)
      startup_error_ = startup_error;
    ++started_;
  }
  ready_.notify_all();
  if (!startup_error)
    run();
}

Result<void> detail::GssPool::acquire()
{
  std::lock_guard lock(mutex_);
  if (sessions_ == options_.capacity)
    return std::unexpected(std::make_error_code(std::errc::no_buffer_space));
  ++sessions_;
  return {};
}

void detail::GssPool::release() noexcept
{
  std::lock_guard lock(mutex_);
  weave::detail::require(sessions_ != 0);
  --sessions_;
}

void detail::GssPool::submit(GssWork &work) noexcept
{
  {
    std::lock_guard lock(mutex_);
    // Each admitted session has at most one call queued or executing, including cleanup.
    weave::detail::require(!stopping_ && queued_ < sessions_);
    enqueue(work);
  }
  ready_.notify_one();
}

void detail::GssPool::enqueue(GssWork &work) noexcept
{
  work.next = nullptr;
  if (last_)
    last_->next = &work;
  else
    first_ = &work;
  last_ = &work;
  ++queued_;
}

void detail::GssPool::retire(std::unique_ptr<OwnedGss> engine) noexcept
{
  engine->owner = this;
  engine->invoke = [](GssWork &work) noexcept {
    auto *owned = static_cast<OwnedGss *>(&work);
    auto *pool = owned->owner;
    delete owned;
    pool->release_retired();
  };
  {
    std::lock_guard lock(mutex_);
    weave::detail::require(!stopping_ && retired_ < sessions_ && queued_ < sessions_);
    ++retired_;
    enqueue(*engine.release());
  }
  ready_.notify_one();
}

void detail::GssPool::release_retired() noexcept
{
  std::lock_guard lock(mutex_);
  weave::detail::require(retired_ != 0 && sessions_ != 0);
  --retired_;
  --sessions_;
}

void detail::GssPool::run() noexcept
{
  for (;;) {
    GssWork *work;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [&] {
        return stopping_ || first_;
      });
      if (!first_)
        return;
      work = first_;
      first_ = work->next;
      if (!first_)
        last_ = nullptr;
      --queued_;
    }
    const auto invoke = work->invoke;
    invoke(*work);
  }
}

detail::GssSession::~GssSession()
{
  auto permit = gate_.try_acquire();
  weave::detail::require(permit.has_value());
  weave::detail::require(!pending_);
  if (engine_)
    pool_->retire(std::move(engine_));

  // Workers never own a pool reference: last-owner destruction joins them here,
  // not on the provider thread that deletes the transferred native context.
  pool_.reset();
}

void detail::GssSession::capture_metadata()
{
  std::lock_guard lock(metadata_);
  complete_ = engine_ && engine_->engine.complete();
  plaintext_limit_ = engine_ ? engine_->engine.plaintext_limit() : 0;
  if (engine_)
    diagnostic_ = engine_->engine.diagnostic();
}

Task<detail::GssToken> detail::GssSession::start(
  const GssContext &context,
  std::string host,
  Authentication method,
  GssOptions options)
{
  auto permit = co_await gate_.acquire();
  weave::detail::require(!pool_ && !engine_ && !pending_);
  if (!context.pool_)
    co_await fail(std::errc::invalid_argument);
  if (auto acquired = context.pool_->acquire(); !acquired)
    co_await fail(acquired.error());
  pool_ = context.pool_;
  engine_ = std::make_unique<OwnedGss>();
  options.credential_cache = pool_->credential_cache();

  Pending pending{pending_};
  co_return co_await provider_call<GssToken>(*pool_, [&]() noexcept {
    auto result = engine_->engine.start(host, method, options);
    capture_metadata();
    return result;
  });
}

Task<detail::GssToken> detail::GssSession::next(std::span<const std::byte> input)
{
  auto permit = co_await gate_.acquire();
  weave::detail::require(pool_ && engine_);
  Pending pending{pending_};
  co_return co_await provider_call<GssToken>(*pool_, [&]() noexcept {
    auto result = engine_->engine.next(input);
    capture_metadata();
    return result;
  });
}

Task<detail::SecretStorage<std::byte>> detail::GssSession::wrap(std::span<const std::byte> input)
{
  auto permit = co_await gate_.acquire();
  weave::detail::require(pool_ && engine_);
  Pending pending{pending_};
  co_return co_await provider_call<SecretStorage<std::byte>>(*pool_, [&]() noexcept {
    auto result = engine_->engine.wrap(input);
    capture_metadata();
    return result;
  });
}

Task<detail::SecretStorage<std::byte>> detail::GssSession::unwrap(std::span<const std::byte> input)
{
  auto permit = co_await gate_.acquire();
  weave::detail::require(pool_ && engine_);
  Pending pending{pending_};
  co_return co_await provider_call<SecretStorage<std::byte>>(*pool_, [&]() noexcept {
    auto result = engine_->engine.unwrap(input);
    capture_metadata();
    return result;
  });
}

Task<void> detail::GssSession::close_impl()
{
  auto permit = gate_.try_acquire();
  weave::detail::require(permit.has_value());
  weave::detail::require(!pending_);
  if (!pool_)
    co_return;
  co_await provider_call<void>(
    *pool_,
    [&]() noexcept -> Result<void> {
      engine_.reset();
      capture_metadata();
      return {};
    },
    true);
  pool_->release();
  pool_.reset();
}

Task<void> detail::GssSession::close()
{
  auto operation = close_impl();
  weave::detail::TaskAccess::bind(operation, {});
  return operation;
}

} // namespace weave::pg
