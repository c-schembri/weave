#include <weave/runtime.hpp>
#include <algorithm>
#include <mutex>
#include <deque>
#include <thread>
#include <vector>

namespace weave {

namespace {

thread_local detail::RuntimeTask *current_task = nullptr;

} // namespace

struct Runtime::Impl {
  struct Worker {
    std::thread thread;
    Context *context = nullptr;
    Error error;
    std::atomic<bool> ready{false};
    std::atomic<bool> idle{false};
    std::mutex queue_mutex;
    std::deque<detail::RuntimeTask *> movable;
    std::deque<detail::RuntimeTask *> pinned;
    bool pinned_turn = true;
  };

  std::vector<std::unique_ptr<Worker>> workers;
  std::mutex submissions;
  std::mutex joining;
  std::atomic<std::size_t> active{0};
  std::atomic<std::size_t> dispatching{0};
  std::atomic<bool> closing{false};
  std::atomic<bool> cancelling{false};
  std::size_t next = 0;
  Scheduler scheduler = Scheduler::worker_affine;
  IoLayout io_layout = IoLayout::sharded;
  std::shared_ptr<detail::IoDomain> io_domain;
  std::atomic<Runtime *> published_runtime{nullptr};

  bool drained() const noexcept
  {
    return active.load(std::memory_order_acquire) == 0 && dispatching.load(std::memory_order_acquire) == 0;
  }

  void enqueue(detail::RuntimeTask &task)
  {
    const auto index = task.worker;
    const bool pinned = task.pinned;
    auto &worker = *workers[index];

    {
      std::lock_guard lock(worker.queue_mutex);
      (pinned ? worker.pinned : worker.movable).push_back(&task);
    }

    // Marking idle before rechecking queues closes the enqueue/park lost-wake race.
    if (worker.idle.exchange(false, std::memory_order_acq_rel))
      detail::ContextAccess::wake(*worker.context);

    if (!pinned) {
      for (std::size_t n = 1; n < workers.size(); ++n) {
        auto &thief = *workers[(index + n) % workers.size()];
        if (thief.idle.exchange(false, std::memory_order_acq_rel)) {
          detail::ContextAccess::wake(*thief.context);
          break;
        }
      }
    }
  }

  detail::RuntimeTask *take(std::size_t index)
  {
    auto &worker = *workers[index];
    {
      std::lock_guard lock(worker.queue_mutex);
      auto prefer_pinned = !worker.pinned.empty() && (worker.pinned_turn || worker.movable.empty());
      auto &queue = prefer_pinned ? worker.pinned : worker.movable;

      if (!queue.empty()) {
        auto *task = queue.front();
        queue.pop_front();
        worker.pinned_turn = !worker.pinned_turn;
        return task;
      }
    }

    for (std::size_t n = 1; n < workers.size(); ++n) {
      auto &victim = *workers[(index + n) % workers.size()];
      std::lock_guard lock(victim.queue_mutex);
      if (!victim.movable.empty()) {
        auto *task = victim.movable.back();
        victim.movable.pop_back();
        return task;
      }
    }

    return nullptr;
  }

  void execute(detail::RuntimeTask &task, std::size_t worker)
  {
    dispatching.fetch_add(1, std::memory_order_acq_rel);
    task.retain(&task);

    detail::Posted *event;
    {
      std::lock_guard lock(task.ready_mutex);
      event = task.first;
      detail::require(event != nullptr && task.scheduled);
      task.first = event->next;
      if (!task.first)
        task.last = nullptr;
      task.worker = worker;
    }

    auto *runtime = static_cast<Runtime *>(task.owner);
    current_task = &task;
    detail::current_executor = &task;
    const auto invoke = event->invoke;
    auto *state = event->state;
    invoke(state);
    detail::current_executor = nullptr;
    current_task = nullptr;

    bool again;
    const bool done = task.scheduler_done;
    {
      std::lock_guard lock(task.ready_mutex);
      again = task.first != nullptr;
      detail::require(!done || !again);
      if (!again)
        task.scheduled = false;
    }

    if (again)
      enqueue(task);
    task.release_scheduled(&task);
    if (done)
      runtime->finished();

    // A requeued continuation can finish on another worker before this dispatch unwinds.
    dispatch_finished();
  }

  void dispatch_finished() noexcept
  {
    auto last_dispatch = dispatching.fetch_sub(1, std::memory_order_acq_rel) == 1;
    if (last_dispatch && closing.load(std::memory_order_acquire) && active.load(std::memory_order_acquire) == 0) {
      std::lock_guard lock(submissions);
      wake_locked();
    }
  }

  void wake_locked() noexcept
  {
    for (auto &worker : workers) {
      if (worker->context)
        detail::ContextAccess::wake(*worker->context);
    }
  }

  void finished() noexcept
  {
    auto last_task = active.fetch_sub(1, std::memory_order_acq_rel) == 1;
    if (last_task && closing.load(std::memory_order_acquire)) {
      std::lock_guard lock(submissions);
      wake_locked();
    }
  }

  void request_stop() noexcept
  {
    std::lock_guard lock(submissions);
    cancelling.store(true, std::memory_order_release);
    for (auto &worker : workers) {
      if (worker->context)
        worker->context->request_stop();
    }
    closing.store(true, std::memory_order_release);
    wake_locked();
  }

  void join()
  {
    std::lock_guard lock_joining(joining);
    {
      std::lock_guard lock(submissions);
      // Close local admission before publishing closing: context tasks share the active count.
      for (auto &worker : workers) {
        if (worker->context)
          detail::ContextAccess::close_submissions(*worker->context);
      }
      closing.store(true, std::memory_order_release);
      wake_locked();
    }

    for (auto &worker : workers) {
      if (worker->thread.joinable())
        worker->thread.join();
    }
  }

  void run(std::size_t index, ContextOptions options)
  {
    auto &worker = *workers[index];
    auto ctx = detail::ContextAccess::create(options, io_domain);
    if (!ctx) {
      worker.error = ctx.error();
      worker.ready.store(true, std::memory_order_release);
      worker.ready.notify_one();
      return;
    }

    detail::ContextAccess::observe(
      *ctx,
      {this,
        [](void *state) noexcept { static_cast<Impl *>(state)->active.fetch_add(1, std::memory_order_relaxed); },
        [](void *state) noexcept { static_cast<Impl *>(state)->finished(); }});
    detail::ContextAccess::enter(*ctx, scheduler == Scheduler::work_stealing ? this : nullptr);
    const detail::SubmissionScope submission{
      this,
      index,
      [](void *state, std::size_t worker, detail::ScheduledSpawn &task) noexcept -> Result<void> {
        auto *runtime = static_cast<Impl *>(state)->published_runtime.load(std::memory_order_acquire);
        detail::require(runtime != nullptr);
        return runtime->submit(task, static_cast<std::size_t>(-1), worker);
      }};
    if (scheduler == Scheduler::work_stealing)
      detail::current_submission = &submission;
    worker.context = &*ctx;
    worker.ready.store(true, std::memory_order_release);
    worker.ready.notify_one();

    for (;;) {
      if (cancelling.load(std::memory_order_acquire))
        detail::ContextAccess::cancel(*ctx);

      if (closing.load(std::memory_order_acquire) && drained())
        break;

      if (scheduler == Scheduler::worker_affine) {
        detail::ContextAccess::poll(*ctx);
        continue;
      }

      unsigned dispatched = 0;
      while (dispatched < 64) {
        auto *task = take(index);
        if (!task)
          break;
        execute(*task, index);
        ++dispatched;
      }

      // Bound ready-work batches so sockets are serviced under sustained CPU load.
      if (dispatched) {
        detail::ContextAccess::poll(*ctx, false);
        continue;
      }

      worker.idle.store(true, std::memory_order_release);
      if (auto *task = take(index)) {
        worker.idle.store(false, std::memory_order_release);
        execute(*task, index);
      } else if (!(closing.load(std::memory_order_acquire) && drained())) {
        detail::ContextAccess::poll(*ctx);
      }
      worker.idle.store(false, std::memory_order_release);
    }
    {
      // Exclude late wake-ups before the worker destroys its IOCP handle.
      std::lock_guard lock(submissions);
      worker.context = nullptr;
    }
    detail::ContextAccess::leave(*ctx);
  }
};

Result<Runtime> Runtime::create(RuntimeOptions options) noexcept
{
  if (options.scheduler != Scheduler::worker_affine && options.scheduler != Scheduler::work_stealing)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (options.io_layout != IoLayout::sharded && options.io_layout != IoLayout::shared)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto impl = std::make_unique<Impl>();
  impl->scheduler = options.scheduler;
  impl->io_layout = options.io_layout;
  const auto count = options.workers ? options.workers : (std::max)(1u, std::thread::hardware_concurrency());
  if (options.io_layout == IoLayout::shared) {
    auto domain = detail::ContextAccess::create_domain(
      count,
      {impl.get(),
        [](void *state) noexcept { static_cast<Impl *>(state)->dispatching.fetch_add(1, std::memory_order_acq_rel); },
        [](void *state) noexcept { static_cast<Impl *>(state)->dispatch_finished(); }});
    if (!domain)
      return std::unexpected(domain.error());
    impl->io_domain = std::move(*domain);
  }
  impl->workers.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    impl->workers.push_back(std::make_unique<Impl::Worker>());

  Error error;
  for (std::size_t i = 0; i < count; ++i) {
    auto &worker = *impl->workers[i];
    worker.thread = std::thread([state = impl.get(), i, options] { state->run(i, options.context); });
    worker.ready.wait(false, std::memory_order_acquire);
    if (worker.error && !error)
      error = worker.error;
  }

  if (error) {
    impl->request_stop();
    impl->join();
    return std::unexpected(error);
  }

  return Result<Runtime>{std::in_place, CreateKey{}, std::move(impl)};
}

Runtime::Runtime(CreateKey, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
  impl_->published_runtime.store(this, std::memory_order_release);
}

Runtime::~Runtime()
{
  shutdown();
}

std::size_t Runtime::worker_count() const noexcept
{
  return impl_->workers.size();
}

Scheduler Runtime::scheduler() const noexcept
{
  return impl_->scheduler;
}

IoLayout Runtime::io_layout() const noexcept
{
  return impl_->io_layout;
}

bool Runtime::stop_requested() const noexcept
{
  return impl_->cancelling.load(std::memory_order_acquire);
}

Result<void> Runtime::submit(detail::RuntimeTask &task, std::size_t worker, std::size_t local_worker)
{
  std::lock_guard lock(impl_->submissions);
  if (impl_->closing.load(std::memory_order_relaxed))
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));

  task.pinned = worker != static_cast<std::size_t>(-1);
  if (!task.pinned) {
    auto *current = current_task;
    auto local = impl_->scheduler == Scheduler::work_stealing && current && current->owner == this;

    if (local_worker != static_cast<std::size_t>(-1))
      worker = local_worker;
    else if (local)
      worker = current->worker;
    else
      worker = impl_->next++ % impl_->workers.size();
  }

  if (worker >= impl_->workers.size())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  task.context = impl_->workers[worker]->context;
  if (task.context->stop_requested())
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));
  task.owner = this;
  task.on_finish = complete;
  task.schedule = detail::schedule;
  task.worker = worker;
  if (impl_->scheduler == Scheduler::work_stealing)
    task.event.executor = &task;

  impl_->active.fetch_add(1, std::memory_order_relaxed);
  detail::ContextAccess::post(*task.context, task.event);
  return {};
}

void detail::schedule(Posted &message) noexcept
{
  auto &task = *static_cast<RuntimeTask *>(message.executor);
  std::lock_guard lock(task.ready_mutex);
  message.next = nullptr;
  if (task.last)
    task.last->next = &message;
  else
    task.first = &message;
  task.last = &message;

  if (!task.scheduled) {
    task.scheduled = true;
    static_cast<Runtime *>(task.owner)->impl_->enqueue(task);
  }
}

void Runtime::finished() noexcept
{
  impl_->finished();
}

void Runtime::complete(detail::SpawnBase *base) noexcept
{
  auto *task = static_cast<detail::RuntimeTask *>(base);
  if (task->event.executor) {
    task->scheduler_done = true;
    task->release_scheduled(task);
    return;
  }
  static_cast<Runtime *>(task->owner)->finished();
  task->release_scheduled(task);
}

void Runtime::request_stop() noexcept
{
  impl_->request_stop();
}

void Runtime::join()
{
  detail::require(!detail::current_context);
  impl_->join();
}

void Runtime::shutdown()
{
  request_stop();
  join();
}

} // namespace weave
