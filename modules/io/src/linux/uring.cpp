#include "uring.hpp"
#include <weave/io/detail/context_access.hpp>
#include <weave/io/detail/trace.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <sys/eventfd.h>
#include <unistd.h>

namespace weave {

namespace {

constexpr u64 wake_key = 1;
constexpr u64 wake_cancel_key = 2;
constexpr u64 cancel_bit = 1;
static_assert(alignof(detail::Operation) > cancel_bit);

Error native_error(int code)
{
  return {code, std::generic_category()};
}

io_uring_sqe *next_entry(io_uring &ring) noexcept
{
  auto *entry = io_uring_get_sqe(&ring);
  detail::require(entry != nullptr);
  return entry;
}

void flush(Context &context) noexcept
{
  auto &state = detail::IoAccess::state(context);
  for (;;) {
    const auto result = io_uring_submit(&state.ring_);
    if (result == -EINTR)
      continue;
    if (result != -EBUSY) {
      detail::require(result >= 0 && io_uring_sq_ready(&state.ring_) == 0);
      return;
    }

    // NODROP backpressure requires consuming CQ entries before retrying submission.
    // Buffer them without resuming user code while an operation's lock is held.
    unsigned head;
    unsigned count = 0;
    io_uring_cqe *completion;
    io_uring_for_each_cqe(&state.ring_, head, completion)
    {
      state.buffered_completions_.push_back({completion->user_data, completion->res});
      ++count;
    }
    detail::require(count != 0);
    io_uring_cq_advance(&state.ring_, count);
  }
}

bool buffered(Context &context, u64 &key, int &result) noexcept
{
  auto &state = detail::IoAccess::state(context);
  if (state.next_completion_ == state.buffered_completions_.size())
    return false;
  const auto completion = state.buffered_completions_[state.next_completion_++];
  key = completion.key;
  result = completion.result;
  if (state.next_completion_ == state.buffered_completions_.size()) {
    state.buffered_completions_.clear();
    state.next_completion_ = 0;
  }
  return true;
}

void arm_wake(Context &context) noexcept
{
  auto &state = detail::IoAccess::state(context);
  auto *entry = next_entry(state.ring_);
  io_uring_prep_read(entry, state.wake_fd_, &state.wake_value_, sizeof(state.wake_value_), 0);
  io_uring_sqe_set_data64(entry, wake_key);
  flush(context);
}

void finish(detail::Operation &operation) noexcept
{
  auto &state = detail::IoAccess::state(*operation.context);
  {
    std::lock_guard lock(state.io_mutex_);
    const auto found = std::find(state.operations_.begin(), state.operations_.end(), &operation);
    detail::require(found != state.operations_.end());
    *found = state.operations_.back();
    state.operations_.pop_back();
  }
  // Publication is the final access; a different worker can destroy the frame.
  detail::post(*operation.context, operation.event);
}

void submit_native(void *address) noexcept
{
  auto &operation = *static_cast<detail::Operation *>(address);
  auto &state = detail::IoAccess::state(*operation.context);
  bool immediate;
  {
    std::lock_guard lock(operation.mutex);
    immediate = operation.cancelled;
    if (immediate) {
      operation.result = -ECANCELED;
      operation.native_done = true;
    } else {
#if defined(WEAVE_PROFILE_RUNTIME)
      const auto started = std::chrono::steady_clock::now();
      if (operation.kind == detail::Operation::Kind::receive)
        detail::IoAccess::count(*operation.context, state.metrics_.read_calls);
      if (operation.kind == detail::Operation::Kind::send)
        detail::IoAccess::count(*operation.context, state.metrics_.write_calls);
#endif
      auto *entry = next_entry(state.ring_);
      operation.prepare(entry, operation);
      io_uring_sqe_set_data(entry, &operation);
      operation.submitted = true;
      detail::IoAccess::count(*operation.context, state.metrics_.submitted);
      flush(*operation.context);
#if defined(WEAVE_PROFILE_RUNTIME)
      const auto elapsed = std::chrono::steady_clock::now() - started;
      const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
      detail::IoAccess::count(*operation.context, state.metrics_.submission_ns, nanoseconds);
      if (operation.kind == detail::Operation::Kind::receive)
        detail::IoAccess::count(*operation.context, state.metrics_.read_submission_ns, nanoseconds);
      if (operation.kind == detail::Operation::Kind::send)
        detail::IoAccess::count(*operation.context, state.metrics_.write_submission_ns, nanoseconds);
#endif
    }
  }
  if (immediate)
    finish(operation);
}

void cancel_native(void *address) noexcept
{
  auto &operation = *static_cast<detail::Operation *>(address);
  bool done;
  {
    std::lock_guard lock(operation.mutex);
    done = operation.native_done;
    if (done) {
      operation.cancel_pending = false;
    } else {
      auto &ring = detail::IoAccess::state(*operation.context).ring_;
      auto *entry = next_entry(ring);
      const auto key = reinterpret_cast<u64>(&operation);
      io_uring_prep_cancel64(entry, key, 0);
      io_uring_sqe_set_data64(entry, key | cancel_bit);
      flush(*operation.context);
    }
  }
  if (done)
    finish(operation);
}

void complete(Context &context, u64 key, int result) noexcept
{
  const bool cancel = (key & cancel_bit) != 0;
  auto &operation = *reinterpret_cast<detail::Operation *>(key & ~cancel_bit);
  bool done;
  {
    std::lock_guard lock(operation.mutex);
    if (cancel) {
      detail::require(operation.cancel_pending);
      detail::require(result == 0 || result == -ENOENT || result == -EALREADY);
      operation.cancel_pending = false;
    } else {
      operation.result = result;
      operation.native_done = true;
      auto &state = detail::IoAccess::state(context);
      detail::IoAccess::count(context, state.metrics_.completed);
    }
    done = operation.native_done && !operation.cancel_pending;
  }
  if (done)
    finish(operation);
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
    const bool use_right = right < heap.size() && heap[right]->deadline < heap[left]->deadline;
    const auto child = use_right ? right : left;
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

std::chrono::nanoseconds detail::TimerQueue::wait_time() noexcept
{
  std::lock_guard lock(mutex);
  if (heap.empty())
    return std::chrono::nanoseconds::max();
  const auto remaining = heap.front()->deadline - std::chrono::steady_clock::now();
  return std::max(std::chrono::nanoseconds::zero(), std::chrono::ceil<std::chrono::nanoseconds>(remaining));
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
  }
  return dispatched;
}

void detail::IoAccess::start_timer(TimerRecord &timer)
{
  check_execution(*timer.context);
  auto &queue = timers(*timer.context);
  bool earliest;
  {
    std::lock_guard lock(queue.mutex);
    earliest = queue.heap.empty() || timer.deadline < queue.heap.front()->deadline;
    queue.insert(timer);
  }
  if (earliest)
    ContextAccess::wake(*timer.context);
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

Result<std::shared_ptr<detail::IoDomain>> detail::ContextAccess::create_domain(std::size_t, TaskObserver) noexcept
{
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
}

Result<Context> detail::ContextAccess::create(ContextOptions options, const std::shared_ptr<IoDomain> &domain) noexcept
{
  if (domain)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  return IoAccess::create_context(options);
}

Result<Context> Context::create(ContextOptions options) noexcept
{
  return detail::IoAccess::create_context(options);
}

Result<Context> detail::IoAccess::create_context(ContextOptions options, unsigned entries) noexcept
{
  auto impl = std::unique_ptr<Context::Impl>{new (std::nothrow) Context::Impl};
  require(impl != nullptr);
  impl->options_ = options;
  io_uring_params parameters{};
  const auto created = io_uring_queue_init_params(entries, &impl->ring_, &parameters);
  if (created < 0)
    return std::unexpected(native_error(-created));

  constexpr unsigned required = IORING_FEAT_EXT_ARG | IORING_FEAT_NODROP;
  if ((parameters.features & required) != required) {
    io_uring_queue_exit(&impl->ring_);
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  }

  impl->wake_fd_ = eventfd(0, EFD_CLOEXEC);
  if (impl->wake_fd_ < 0) {
    const auto error = native_error(errno);
    io_uring_queue_exit(&impl->ring_);
    return std::unexpected(error);
  }
  return Result<Context>{std::in_place, Context::CreateKey{}, std::move(impl)};
}

Context::Context(CreateKey, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
  arm_wake(*this);
}

Context::~Context()
{
  check_thread();
  detail::require(!impl_->running_);
  shutdown();
  detail::require(impl_->handles_ == 0 && impl_->metrics_.submitted == impl_->metrics_.completed);
  detail::require(impl_->ready_first_ == nullptr && impl_->operations_.empty() && impl_->timers_.heap.empty());

  // Closing a descriptor does not cancel io_uring requests. Drain the wake read too.
  auto *entry = next_entry(impl_->ring_);
  io_uring_prep_cancel64(entry, wake_key, 0);
  io_uring_sqe_set_data64(entry, wake_cancel_key);
  flush(*this);
  unsigned drained = 0;
  while (drained != 2) {
    u64 key;
    int bytes;
    if (buffered(*this, key, bytes)) {
      detail::require(key == wake_key || key == wake_cancel_key);
      ++drained;
      continue;
    }
    io_uring_cqe *completion;
    const auto result = io_uring_wait_cqe(&impl_->ring_, &completion);
    if (result == -EINTR)
      continue;
    detail::require(result == 0);
    detail::require(completion->user_data == wake_key || completion->user_data == wake_cancel_key);
    io_uring_cqe_seen(&impl_->ring_, completion);
    ++drained;
  }
  io_uring_queue_exit(&impl_->ring_);
  ::close(impl_->wake_fd_);
}

void Context::enter(void *scheduler_group) noexcept
{
  check_thread();
  detail::require(!impl_->running_ && !detail::current_context);
  impl_->scheduler_group_ = scheduler_group;
  impl_->running_ = true;
  detail::current_context = this;
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
  const auto previous = impl_->active_.fetch_sub(1, std::memory_order_acq_rel);
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
  constexpr u64 value = 1;
  ssize_t result;
  do {
    result = ::write(context.impl_->wake_fd_, &value, sizeof(value));
  } while (result < 0 && errno == EINTR);
  require(result == sizeof(value));
}

void Context::check_thread() const noexcept
{
  const bool correct_owner = impl_->owner_ == std::this_thread::get_id();
  const bool valid = impl_->scheduler_group_ && detail::current_context;
  const bool correct_group = valid && detail::current_context->impl_->scheduler_group_ == impl_->scheduler_group_;
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

Result<bool> detail::IoAccess::attach(Context &context, std::uintptr_t handle, bool)
{
  check_thread(context);
  auto &state = *context.impl_;
  std::lock_guard lock(state.io_mutex_);
  if (state.stopping_)
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));
  ++state.handles_;
  state.registered_handles_.push_back(handle);
  return false;
}

Result<void> detail::IoAccess::close(
  Context &context,
  std::uintptr_t handle,
  Error (*close_native)(std::uintptr_t) noexcept)
{
  check_thread(context);
  auto &state = *context.impl_;
  std::lock_guard lock(state.io_mutex_);
  if (auto error = close_native(handle))
    return std::unexpected(error);
  const auto found = std::find(state.registered_handles_.begin(), state.registered_handles_.end(), handle);
  require(found != state.registered_handles_.end());
  *found = state.registered_handles_.back();
  state.registered_handles_.pop_back();
  --state.handles_;
  return {};
}

void detail::IoAccess::submit(Operation &operation) noexcept
{
  auto &state = *operation.context->impl_;
  {
    std::lock_guard lock(state.io_mutex_);
    state.operations_.push_back(&operation);
  }
  operation.submission.state = &operation;
  operation.submission.invoke = submit_native;
  operation.cancellation.state = &operation;
  operation.cancellation.invoke = cancel_native;
  {
    std::lock_guard lock(operation.mutex);
    ContextAccess::post(*operation.context, operation.submission);
  }
}

void detail::IoAccess::cancel(Operation &operation) noexcept
{
  std::lock_guard lock(operation.mutex);
  operation.cancelled = true;
  if (operation.submitted && !operation.native_done && !operation.cancel_pending) {
    operation.cancel_pending = true;
    ContextAccess::post(*operation.context, operation.cancellation);
  }
}

void detail::IoAccess::cancel_socket(Context &context, int socket) noexcept
{
  auto &state = *context.impl_;
  std::lock_guard lock(state.io_mutex_);
  for (auto *operation : state.operations_) {
    if (operation->socket == socket)
      cancel(*operation);
  }
}

void Context::cancel_pending() noexcept
{
  // Each operation observes the Context token, including shielded tasks.
}

void Context::post(detail::Posted &message) noexcept
{
  if (message.executor) {
    message.executor->schedule(message);
    return;
  }
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
}

void Context::Yield::await_suspend(std::coroutine_handle<> continuation) noexcept
{
  detail::IoAccess::check_execution(context);
  message.executor = detail::current_executor;
  message.state = continuation.address();
  message.invoke = [](void *address) noexcept {
    std::coroutine_handle<>::from_address(address).resume();
  };
  context.post(message);
}

void Context::poll(bool wait)
{
  detail::inline_budget = 0;
  bool dispatched = impl_->timers_.dispatch_due();
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

  count(impl_->metrics_.dequeue_calls);
  if (!impl_->buffered_completions_.empty()) {
    for (unsigned n = 0; n < 64; ++n) {
      u64 key;
      int bytes;
      if (!buffered(*this, key, bytes))
        break;
      if (key == wake_key) {
        detail::require(bytes == sizeof(impl_->wake_value_));
        arm_wake(*this);
      } else {
        complete(*this, key, bytes);
      }
    }
    return;
  }
  io_uring_cqe *completion = nullptr;
  int result;
  const auto delay = impl_->timers_.wait_time();
#if defined(WEAVE_PROFILE_RUNTIME)
  const auto dequeue_started = std::chrono::steady_clock::now();
#endif
  if (!wait || dispatched) {
    result = io_uring_peek_cqe(&impl_->ring_, &completion);
  } else if (delay == std::chrono::nanoseconds::max()) {
    result = io_uring_wait_cqe(&impl_->ring_, &completion);
  } else {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(delay);
    __kernel_timespec timeout{seconds.count(), (delay - seconds).count()};
    result = io_uring_wait_cqe_timeout(&impl_->ring_, &completion, &timeout);
  }
#if defined(WEAVE_PROFILE_RUNTIME)
  const auto elapsed = std::chrono::steady_clock::now() - dequeue_started;
  count(impl_->metrics_.dequeue_ns, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
#endif
  if (result == -EAGAIN || result == -ETIME || result == -EINTR)
    return;
  detail::require(result == 0);

  for (unsigned n = 0; n < 64; ++n) {
    const auto key = completion->user_data;
    const auto bytes = completion->res;
    io_uring_cqe_seen(&impl_->ring_, completion);
    if (key == wake_key) {
      detail::require(bytes == sizeof(impl_->wake_value_));
      arm_wake(*this);
    } else {
      complete(*this, key, bytes);
    }
    result = io_uring_peek_cqe(&impl_->ring_, &completion);
    if (result == -EAGAIN)
      break;
    detail::require(result == 0);
  }
}

} // namespace weave
