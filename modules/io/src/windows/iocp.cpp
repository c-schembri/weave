#include "iocp.hpp"
#include <weave/io/detail/context_access.hpp>
#include <algorithm>
#include <mutex>
#include <weave/io/detail/trace.hpp>
#include <type_traits>
#if defined(WEAVE_PROFILE_RUNTIME)
#include <chrono>
#endif

namespace weave {

namespace {

constexpr ULONG_PTR posted_key = 1;
constexpr ULONG_PTR wake_key = 2;

Error win_error(DWORD code)
{
  return {static_cast<int>(code), std::system_category()};
}

} // namespace

void detail::TimerQueue::swap(std::size_t a, std::size_t b) noexcept
{
  std::swap(heap[a], heap[b]);
  heap[a]->index = a;
  heap[b]->index = b;
}

void detail::TimerQueue::repair(std::size_t index) noexcept
{
  while (index > 0) {
    const auto parent = (index - 1) / 2;
    if (heap[parent]->deadline <= heap[index]->deadline)
      break;
    swap(parent, index);
    index = parent;
  }
  for (;;) {
    const auto left = index * 2 + 1;
    if (left >= heap.size())
      break;
    const auto right = left + 1;
    const auto child = right < heap.size() && heap[right]->deadline < heap[left]->deadline ? right : left;
    if (heap[index]->deadline <= heap[child]->deadline)
      break;
    swap(index, child);
    index = child;
  }
}

void detail::TimerQueue::insert(TimerRecord &timer)
{
  timer.index = heap.size();
  heap.push_back(&timer);
  repair(timer.index);
}

void detail::TimerQueue::remove(TimerRecord &timer) noexcept
{
  const auto index = timer.index;
  require(index < heap.size() && heap[index] == &timer);
  swap(index, heap.size() - 1);
  heap.pop_back();
  timer.index = std::numeric_limits<std::size_t>::max();
  if (index < heap.size())
    repair(index);
}

DWORD detail::TimerQueue::wait_time() noexcept
{
  std::lock_guard lock(mutex);
  if (heap.empty())
    return INFINITE;
  const auto remaining = heap.front()->deadline - std::chrono::steady_clock::now();
  if (remaining <= std::chrono::steady_clock::duration::zero())
    return 0;
  const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
  return static_cast<DWORD>(std::min(milliseconds, static_cast<decltype(milliseconds)>(INFINITE - 1)));
}

bool detail::TimerQueue::dispatch_due() noexcept
{
  bool dispatched = false;
  for (unsigned n = 0; n < 64; ++n) {
    Posted *event;
    Context *context;
    {
      std::lock_guard lock(mutex);
      if (heap.empty() || heap.front()->deadline > std::chrono::steady_clock::now())
        break;
      auto *timer = heap.front();
      event = &timer->event;
      context = timer->context;
      remove(*timer);
    }
    dispatched = true;
    post(*context, *event);
    // Publication can resume and destroy the timer on another worker.
  }
  return dispatched;
}

void detail::IoDomain::wake_waiters() noexcept
{
  std::lock_guard lock(waiters_mutex);
  for (auto thread : waiters)
    require(QueueUserAPC([](ULONG_PTR) {}, thread, 0) != 0);
}

void detail::IoAccess::start_timer(TimerRecord &timer)
{
  auto &context = *timer.context;
  check_execution(context);
  auto &queue = timers(context);
  bool earliest;
  {
    std::lock_guard lock(queue.mutex);
    earliest = queue.heap.empty() || timer.deadline < queue.heap.front()->deadline;
    queue.insert(timer);
  }
  if (earliest) {
    if (context.impl_->domain_)
      context.impl_->domain_->wake_waiters();
    else
      ContextAccess::wake(context);
  }
}

void detail::IoAccess::cancel_timer(TimerRecord &timer) noexcept
{
  auto &queue = timers(*timer.context);
  Posted *event;
  Context *context;
  {
    std::lock_guard lock(queue.mutex);
    if (timer.index == std::numeric_limits<std::size_t>::max())
      return;
    queue.remove(timer);
    timer.error = std::make_error_code(std::errc::operation_canceled);
    event = &timer.event;
    context = timer.context;
  }
  post(*context, *event);
}

detail::IoDomain::~IoDomain()
{
  require(timers.heap.empty() && waiters.empty());
  if (port)
    CloseHandle(port);
}

Result<std::shared_ptr<detail::IoDomain>> detail::ContextAccess::create_domain(
  std::size_t concurrency,
  TaskObserver collectors) noexcept
{
  auto domain = std::make_shared<IoDomain>();
  domain->collectors = collectors;
  domain->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, static_cast<DWORD>(concurrency));
  if (!domain->port)
    return std::unexpected(win_error(GetLastError()));
  return domain;
}

Result<Context> detail::ContextAccess::create(ContextOptions options, const std::shared_ptr<IoDomain> &domain) noexcept
{
  return IoAccess::create_context(options, &CreateIoCompletionPort, domain);
}

Result<Context> Context::create(ContextOptions options) noexcept
{
  return detail::IoAccess::create_context(options, &CreateIoCompletionPort);
}

Result<Context> detail::IoAccess::create_context(
  ContextOptions options,
  decltype(&CreateIoCompletionPort) create_port,
  const std::shared_ptr<IoDomain> &domain) noexcept
{
  auto impl = std::unique_ptr<Context::Impl>{new (std::nothrow) Context::Impl};
  require(impl != nullptr);
  impl->options_ = options;
  impl->domain_ = domain;
  impl->port_ = domain ? domain->port : create_port(INVALID_HANDLE_VALUE, nullptr, 0, 1);
  if (!impl->port_)
    return std::unexpected(win_error(GetLastError()));

  if (domain &&
    !DuplicateHandle(
      GetCurrentProcess(),
      GetCurrentThread(),
      GetCurrentProcess(),
      &impl->thread_,
      0,
      FALSE,
      DUPLICATE_SAME_ACCESS))
    return std::unexpected(win_error(GetLastError()));

  return Result<Context>{std::in_place, Context::CreateKey{}, std::move(impl)};
}

Context::Context(CreateKey, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
  if (impl_->domain_) {
    std::lock_guard lock(impl_->domain_->waiters_mutex);
    impl_->domain_->waiters.push_back(impl_->thread_);
  }
}

Context::~Context()
{
  check_thread();
  detail::require(!impl_->running_);
  shutdown();
  detail::require(impl_->handles_ == 0 && impl_->metrics_.submitted == impl_->metrics_.completed);
  detail::require(impl_->ready_first_ == nullptr);
  detail::require(impl_->timers_.heap.empty());
  if (impl_->thread_) {
    std::lock_guard lock(impl_->domain_->waiters_mutex);
    std::erase(impl_->domain_->waiters, impl_->thread_);
    CloseHandle(impl_->thread_);
  }
  if (!impl_->domain_)
    CloseHandle(impl_->port_);
}

void Context::enter(void *scheduler_group) noexcept
{
  check_thread();
  detail::require(!impl_->running_ && !detail::current_context);
  impl_->scheduler_group_ = scheduler_group;
  impl_->running_ = true;
  detail::current_context = this;
  if (stop_requested())
    cancel_pending();
}

void Context::leave() noexcept
{
  impl_->running_ = false;
  detail::current_submission = nullptr;
  detail::current_context = nullptr;
}

bool Context::stop_requested() const noexcept
{
  return impl_->stopping_.load(std::memory_order_acquire);
}

void Context::run()
{
  enter(nullptr);
  while (!stop_requested() || impl_->active_.load(std::memory_order_acquire) != 0)
    poll();
  leave();
}

void Context::request_stop() noexcept
{
  bool wake;
  {
    std::lock_guard lock(impl_->submissions_);
    impl_->closing_ = true;
    wake = !impl_->stopping_.exchange(true, std::memory_order_acq_rel);
  }
  impl_->stop_source_.cancel();
  if (wake)
    detail::ContextAccess::wake(*this);
}

void Context::shutdown()
{
  check_thread();
  detail::require(!impl_->running_);
  request_stop();
  if (impl_->active_.load(std::memory_order_acquire) != 0)
    run();
  else
    cancel_pending();
}

Result<void> Context::submit(detail::SpawnBase &task)
{
  std::lock_guard lock(impl_->submissions_);
  if (impl_->closing_)
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));

  task.context = this;
  task.on_finish = [](detail::SpawnBase *task) noexcept {
    task->context->finished();
    task->release_scheduled(task);
  };
  impl_->active_.fetch_add(1, std::memory_order_relaxed);
  if (impl_->observer_.spawned)
    impl_->observer_.spawned(impl_->observer_.state);
  post(task.event);
  return {};
}

void Context::finished() noexcept
{
  auto previous = impl_->active_.fetch_sub(1, std::memory_order_acq_rel);
  detail::require(previous != 0);
  if (impl_->observer_.finished)
    impl_->observer_.finished(impl_->observer_.state);
}

void Context::close_submissions() noexcept
{
  std::lock_guard lock(impl_->submissions_);
  impl_->closing_ = true;
}

void Context::observe(detail::TaskObserver observer) noexcept
{
  check_thread();
  detail::require(!impl_->running_ && impl_->active_.load(std::memory_order_relaxed) == 0);
  impl_->observer_ = observer;
}

void detail::post(Context &context, Posted &message) noexcept
{
  ContextAccess::post(context, message);
}

CancelToken detail::context_cancellation(Context &context) noexcept
{
  return IoAccess::state(context).stop_source_.token();
}

void detail::ContextAccess::wake(Context &context) noexcept
{
  trace(TraceEvent::wake, &context);
  if (context.impl_->domain_) {
    // Alertable IOCP waits allow a targeted wake without a polling timeout or a second driver thread.
    auto queued = QueueUserAPC([](ULONG_PTR) {}, context.impl_->thread_, 0);
    require(queued != 0);
    return;
  }
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
  if (impl_->scheduler_group_ || impl_->domain_)
    std::atomic_ref<u64>(counter).fetch_add(amount, std::memory_order_relaxed);
  else
    counter += amount;
}

Context::Metrics Context::metrics() const noexcept
{
  if (!impl_->scheduler_group_ && !impl_->domain_)
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

Result<bool> detail::IoAccess::attach(Context &context, std::uintptr_t handle, bool skip_success)
{
  context.check_thread();
  auto &state = *context.impl_;
  std::unique_lock registry(state.io_mutex_, std::defer_lock);
  if (state.scheduler_group_)
    registry.lock();

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
  state.registered_handles_.push_back(handle);
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

  auto found = std::find(state.registered_handles_.begin(), state.registered_handles_.end(), handle);
  require(found != state.registered_handles_.end());
  *found = state.registered_handles_.back();
  state.registered_handles_.pop_back();

  --state.handles_;
  return {};
}

void Context::cancel_pending() noexcept
{
  check_thread();
  std::unique_lock registry(impl_->io_mutex_, std::defer_lock);
  if (impl_->scheduler_group_)
    registry.lock();

  if (impl_->cancelled_)
    return;

  impl_->cancelled_ = true;
  for (auto socket : impl_->registered_handles_) {
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

  if (impl_->domain_) {
    bool wake;
    {
      std::lock_guard lock(impl_->ready_mutex_);
      wake = impl_->ready_first_ == nullptr;
      message.next = nullptr;
      if (impl_->ready_last_)
        impl_->ready_last_->next = &message;
      else
        impl_->ready_first_ = &message;
      impl_->ready_last_ = &message;
    }
    if (wake && impl_->owner_ != std::this_thread::get_id())
      detail::ContextAccess::wake(*this);
    return;
  }

  auto posted = PostQueuedCompletionStatus(impl_->port_, 0, posted_key, reinterpret_cast<OVERLAPPED *>(&message));
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
  if (stop_requested())
    cancel_pending();
  impl_->inline_budget_ = 0;
  detail::inline_budget = 0;
  const auto collectors = impl_->domain_ ? impl_->domain_->collectors : detail::TaskObserver{};
  // Timer dispatch has the same cross-worker lifetime requirement as native completion dispatch.
  if (collectors.spawned)
    collectors.spawned(collectors.state);
  bool dispatched = detail::IoAccess::timers(*this).dispatch_due();
  if (collectors.finished)
    collectors.finished(collectors.state);
  if (impl_->domain_) {
    for (unsigned n = 0; n < 64; ++n) {
      detail::Posted *message;
      {
        std::lock_guard lock(impl_->ready_mutex_);
        message = impl_->ready_first_;
        if (!message)
          break;
        impl_->ready_first_ = message->next;
        if (!impl_->ready_first_)
          impl_->ready_last_ = nullptr;
      }
      dispatched = true;
      const auto invoke = message->invoke;
      auto *state = message->state;
      invoke(state);
    }
  }
  ULONG count = 0;
  this->count(impl_->metrics_.dequeue_calls);
#if defined(WEAVE_PROFILE_RUNTIME)
  const auto dequeue_start = std::chrono::steady_clock::now();
#endif
  detail::trace(detail::TraceEvent::io_wait_begin, this, wait && !dispatched);
  auto dequeued = GetQueuedCompletionStatusEx(
    impl_->port_,
    impl_->completions_.data(),
    static_cast<ULONG>(impl_->completions_.size()),
    &count,
    wait && !dispatched ? detail::IoAccess::timers(*this).wait_time() : 0,
    impl_->domain_ ? TRUE : FALSE);
  detail::trace(detail::TraceEvent::io_wait_end, this, count);
  if (!dequeued) {
    const auto error = GetLastError();
    if (error == WAIT_TIMEOUT || (impl_->domain_ && error == WAIT_IO_COMPLETION))
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

  // Keep all worker Contexts alive until cross-worker completion publication has unwound.
  if (collectors.spawned)
    collectors.spawned(collectors.state);

  for (ULONG i = 0; i < count; ++i) {
    const auto entry = impl_->completions_[i];
    if (entry.lpCompletionKey == wake_key)
      continue;

    if (entry.lpCompletionKey == posted_key) {
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
    auto &owner = *op->context;
    detail::trace(
      detail::TraceEvent::io_complete,
      op->event.state,
      reinterpret_cast<std::uintptr_t>(op->event.executor));
    owner.count(owner.impl_->metrics_.completed);

    // Resumption may destroy this operation. Do not touch op afterward.
    if (op->event.executor) {
      const auto executor = op->event.executor;
      const auto dispatch = executor->dispatch_completion ? executor->dispatch_completion : executor->schedule;
      dispatch(op->event);
    } else if (&owner == this)
      op->event.invoke(op->event.state);
    else
      owner.post(op->event);
  }
  if (collectors.finished)
    collectors.finished(collectors.state);
}

} // namespace weave
