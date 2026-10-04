#include "iocp.hpp"
#include <weave/io/detail/context_access.hpp>
#include <algorithm>
#include <mutex>
#include <type_traits>
#if defined(WEAVE_PROFILE_RUNTIME)
#include <chrono>
#endif

namespace weave {

static Error win_error(DWORD code)
{
  return {static_cast<int>(code), std::system_category()};
}

Context::Context(ContextOptions options) noexcept : impl_(new (std::nothrow) Impl)
{
  detail::require(impl_ != nullptr);
  impl_->options_ = options;
  impl_->port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (!impl_->port_)
    impl_->error_ = win_error(GetLastError());
}

Context::~Context()
{
  check_thread();
  detail::require(!impl_->running_ && impl_->handles_ == 0 && impl_->metrics_.submitted == impl_->metrics_.completed);
  if (impl_->port_)
    CloseHandle(impl_->port_);
}

void Context::enter(bool managed, void *scheduler_group) noexcept
{
  check_thread();
  detail::require(!impl_->running_ && !impl_->error_ && !detail::current_context);
  impl_->managed_ = managed;
  impl_->scheduler_group_ = scheduler_group;
  impl_->running_ = true;
  detail::current_context = this;
}

void Context::leave() noexcept
{
  impl_->running_ = false;
  detail::current_context = nullptr;
}

bool Context::stop_requested() const noexcept
{
  return impl_->stopping_.load(std::memory_order_acquire);
}

void detail::ContextAccess::wake(Context &context) noexcept
{
  auto posted = PostQueuedCompletionStatus(context.impl_->port_, 0, wake_key, nullptr);
  require(posted != FALSE);
}

void Context::check_thread() const noexcept
{
  auto correct_owner = impl_->owner_ == std::this_thread::get_id();
  auto valid = impl_->scheduler_group_ && detail::current_context;
  auto correct_group = valid && detail::current_context->impl_->scheduler_group_ == impl_->scheduler_group_;

  detail::require(correct_owner || correct_group);
}

void Context::count(u64 &counter, u64 amount) noexcept
{
  if (impl_->scheduler_group_)
    std::atomic_ref<u64>(counter).fetch_add(amount, std::memory_order_relaxed);
  else
    counter += amount;
}

Context::Metrics Context::metrics() const noexcept
{
  if (!impl_->scheduler_group_)
    return impl_->metrics_;

  Metrics result;
  constexpr std::array fields{
    &Metrics::submitted,
    &Metrics::completed,
    &Metrics::dequeue_calls,
    &Metrics::inline_completions,
    &Metrics::fairness_posts,
    &Metrics::read_calls,
    &Metrics::write_calls,
    &Metrics::read_bytes,
    &Metrics::write_bytes,
    &Metrics::immediate_successes,
    &Metrics::submission_ns,
    &Metrics::read_submission_ns,
    &Metrics::write_submission_ns,
    &Metrics::dequeue_ns};

  for (auto field : fields) {
    auto counter = std::atomic_ref<u64>(const_cast<u64 &>(impl_->metrics_.*field));
    result.*field = counter.load(std::memory_order_relaxed);
  }

  return result;
}

Result<void> Context::status() const noexcept
{
  if (impl_->error_)
    return std::unexpected(impl_->error_);

  return {};
}

Result<bool> detail::IoAccess::attach(Context &context, std::uintptr_t handle, bool skip_success)
{
  context.check_thread();
  auto &state = *context.impl_;
  std::unique_lock registry(state.io_mutex_, std::defer_lock);
  if (state.scheduler_group_)
    registry.lock();

  if (state.error_)
    return std::unexpected(state.error_);
  if (state.stopping_)
    return std::unexpected(win_error(ERROR_OPERATION_ABORTED));

  auto native = reinterpret_cast<HANDLE>(handle);
  if (!CreateIoCompletionPort(native, state.port_, 0, 0))
    return std::unexpected(win_error(GetLastError()));

  bool skipping = false;
  if (skip_success && state.options_.skip_successful_completions) {
    auto enabled = SetFileCompletionNotificationModes(
      native,
      FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE);
    skipping = enabled != FALSE;
  }

  ++state.handles_;
  if (state.managed_)
    state.managed_handles_.push_back(handle);
  return skipping;
}

Result<void> detail::IoAccess::close(
  Context &context,
  std::uintptr_t handle,
  Error (*close_native)(std::uintptr_t) noexcept)
{
  context.check_thread();
  auto &state = *context.impl_;
  std::unique_lock registry(state.io_mutex_, std::defer_lock);
  if (state.scheduler_group_)
    registry.lock();

  // Hold the registry lock through close so cancellation cannot see a recycled handle.
  if (auto error = close_native(handle))
    return std::unexpected(error);

  if (state.managed_) {
    auto found = std::find(state.managed_handles_.begin(), state.managed_handles_.end(), handle);
    require(found != state.managed_handles_.end());
    *found = state.managed_handles_.back();
    state.managed_handles_.pop_back();
  }

  --state.handles_;
  return {};
}

void Context::cancel_pending() noexcept
{
  check_thread();
  std::unique_lock registry(impl_->io_mutex_, std::defer_lock);
  if (impl_->scheduler_group_)
    registry.lock();

  if (impl_->stopping_)
    return;

  impl_->stopping_ = true;
  for (auto socket : impl_->managed_handles_) {
    if (!CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr))
      detail::require(GetLastError() == ERROR_NOT_FOUND);
  }
}

void Context::post(detail::Posted &message) noexcept
{
  if (message.executor) {
    message.executor->schedule(message);
    return;
  }

  auto posted = PostQueuedCompletionStatus(
    impl_->port_,
    0,
    detail::posted_key,
    reinterpret_cast<OVERLAPPED *>(&message));
  detail::require(posted != FALSE);
}

void Context::Yield::await_suspend(std::coroutine_handle<> continuation) noexcept
{
  context.check_thread();
  detail::require(detail::current_context == &context || (context.impl_->scheduler_group_ && detail::current_executor));

  message.executor = detail::current_executor;
  message.state = continuation.address();
  message.invoke = [](void *address) noexcept { std::coroutine_handle<>::from_address(address).resume(); };
  context.post(message);
}

void Context::poll(bool wait)
{
  impl_->inline_budget_ = 0;
  detail::inline_budget = 0;
  ULONG count = 0;
  this->count(impl_->metrics_.dequeue_calls);
#if defined(WEAVE_PROFILE_RUNTIME)
  const auto dequeue_start = std::chrono::steady_clock::now();
#endif
  auto dequeued = GetQueuedCompletionStatusEx(
    impl_->port_,
    impl_->completions_.data(),
    static_cast<ULONG>(impl_->completions_.size()),
    &count,
    wait ? INFINITE : 0,
    FALSE);
  if (!dequeued) {
    if (GetLastError() == WAIT_TIMEOUT)
      return;

    // Port failure is a runtime invariant failure, not an individual I/O error.
    std::abort();
  }
#if defined(WEAVE_PROFILE_RUNTIME)
  const auto dequeue_time = std::chrono::steady_clock::now() - dequeue_start;
  const auto dequeue_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dequeue_time).count();
  this->count(impl_->metrics_.dequeue_ns, dequeue_ns);
#endif
  static_assert(std::is_standard_layout_v<detail::Operation>);
  static_assert(offsetof(detail::Operation, overlapped) == 0);

  for (ULONG i = 0; i < count; ++i) {
    const auto entry = impl_->completions_[i];
    if (entry.lpCompletionKey == detail::wake_key)
      continue;

    if (entry.lpCompletionKey == detail::posted_key) {
      static_assert(std::is_standard_layout_v<detail::Posted>);
      auto *message = reinterpret_cast<detail::Posted *>(entry.lpOverlapped);
      const auto invoke = message->invoke;
      auto *state = message->state;
      invoke(state);
      continue;
    }

    auto *op = reinterpret_cast<detail::Operation *>(entry.lpOverlapped);
    detail::require(op != nullptr);
    op->transferred = entry.dwNumberOfBytesTransferred;
    op->failed = entry.Internal != 0;
    this->count(impl_->metrics_.completed);

    // Resumption may destroy this operation. Do not touch op afterward.
    if (op->event.executor)
      op->event.executor->schedule(op->event);
    else
      op->event.invoke(op->event.state);
  }
}

} // namespace weave
