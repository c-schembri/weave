#include <weave/runtime.hpp>
#include <algorithm>
#include <mutex>
#include <deque>
#include <thread>
#include <vector>

namespace weave {

struct Runtime::Impl {
  struct Worker {
    std::thread thread;
    Context *context = nullptr;
    Error error;
    std::atomic<bool> ready{false};
    std::atomic<bool> idle{false};
    std::mutex queue_mutex;
    std::deque<detail::SpawnBase *> movable;
    std::deque<detail::SpawnBase *> pinned;
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
  Error error;
  Scheduler scheduler = Scheduler::worker_affine;

  bool drained() const noexcept
  {
    return active.load(std::memory_order_acquire) == 0 && dispatching.load(std::memory_order_acquire) == 0;
  }

  void enqueue(detail::SpawnBase &task)
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

  detail::SpawnBase *take(std::size_t index)
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

  void execute(detail::SpawnBase &task, std::size_t worker)
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

    auto *runtime = task.runtime;
    detail::current_task = &task;
    detail::current_executor = &task;
    const auto invoke = event->invoke;
    auto *state = event->state;
    invoke(state);
    detail::current_executor = nullptr;
    detail::current_task = nullptr;

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

  void run(std::size_t index, ContextOptions options)
  {
    auto &worker = *workers[index];
    Context context(options);
    auto status = context.status();
    if (!status) {
      worker.error = status.error();
      worker.ready.store(true, std::memory_order_release);
      worker.ready.notify_one();
      return;
    }

    detail::ContextAccess::enter(context, scheduler == Scheduler::work_stealing ? this : nullptr);
    worker.context = &context;
    worker.ready.store(true, std::memory_order_release);
    worker.ready.notify_one();

    for (;;) {
      if (cancelling.load(std::memory_order_acquire))
        detail::ContextAccess::cancel(context);

      if (closing.load(std::memory_order_acquire) && drained())
        break;

      if (scheduler == Scheduler::worker_affine) {
        detail::ContextAccess::poll(context);
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
        detail::ContextAccess::poll(context, false);
        continue;
      }

      worker.idle.store(true, std::memory_order_release);
      if (auto *task = take(index)) {
        worker.idle.store(false, std::memory_order_release);
        execute(*task, index);
      } else if (!(closing.load(std::memory_order_acquire) && drained())) {
        detail::ContextAccess::poll(context);
      }
      worker.idle.store(false, std::memory_order_release);
    }
    {
      // Exclude late wake-ups before the worker destroys its IOCP handle.
      std::lock_guard lock(submissions);
      worker.context = nullptr;
    }
    detail::ContextAccess::leave(context);
  }
};

Runtime::Runtime(RuntimeOptions options) : impl_(std::make_unique<Impl>())
{
  impl_->scheduler = options.scheduler;
  if (options.scheduler != Scheduler::worker_affine && options.scheduler != Scheduler::work_stealing) {
    impl_->error = std::make_error_code(std::errc::invalid_argument);
    return;
  }

  const auto count = options.workers ? options.workers : (std::max)(1u, std::thread::hardware_concurrency());
  impl_->workers.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    impl_->workers.push_back(std::make_unique<Impl::Worker>());

  for (std::size_t i = 0; i < count; ++i) {
    auto &worker = *impl_->workers[i];
    worker.thread = std::thread([this, i, options] { impl_->run(i, options.context); });
    worker.ready.wait(false, std::memory_order_acquire);
    if (worker.error && !impl_->error)
      impl_->error = worker.error;
  }

  if (impl_->error)
    shutdown();
}

Runtime::~Runtime()
{
  shutdown();
}

Result<void> Runtime::status() const noexcept
{
  if (impl_->error)
    return std::unexpected(impl_->error);
  return {};
}

std::size_t Runtime::worker_count() const noexcept
{
  return impl_->workers.size();
}

Scheduler Runtime::scheduler() const noexcept
{
  return impl_->scheduler;
}

bool Runtime::stop_requested() const noexcept
{
  return impl_->cancelling.load(std::memory_order_acquire);
}

Result<void> Runtime::submit(detail::SpawnBase &task, std::size_t worker)
{
  std::lock_guard lock(impl_->submissions);
  if (impl_->error)
    return std::unexpected(impl_->error);

  if (impl_->closing.load(std::memory_order_relaxed))
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));

  task.pinned = worker != static_cast<std::size_t>(-1);
  if (!task.pinned) {
    auto *current = detail::current_task;
    auto local = impl_->scheduler == Scheduler::work_stealing && current && current->runtime == this;

    if (local)
      worker = current->worker;
    else
      worker = impl_->next++ % impl_->workers.size();
  }

  if (worker >= impl_->workers.size())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  task.context = impl_->workers[worker]->context;
  task.runtime = this;
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
  auto &task = *static_cast<SpawnBase *>(message.executor);
  std::lock_guard lock(task.ready_mutex);
  message.next = nullptr;
  if (task.last)
    task.last->next = &message;
  else
    task.first = &message;
  task.last = &message;

  if (!task.scheduled) {
    task.scheduled = true;
    task.runtime->impl_->enqueue(task);
  }
}

void Runtime::finished() noexcept
{
  auto last_task = impl_->active.fetch_sub(1, std::memory_order_acq_rel) == 1;
  if (last_task && impl_->closing.load(std::memory_order_acquire)) {
    std::lock_guard lock(impl_->submissions);
    impl_->wake_locked();
  }
}

void Runtime::request_stop() noexcept
{
  std::lock_guard lock(impl_->submissions);
  impl_->closing.store(true, std::memory_order_release);
  impl_->cancelling.store(true, std::memory_order_release);
  impl_->wake_locked();
}

void Runtime::join()
{
  detail::require(!detail::current_context);
  std::lock_guard joining(impl_->joining);
  {
    std::lock_guard lock(impl_->submissions);
    impl_->closing.store(true, std::memory_order_release);
    impl_->wake_locked();
  }

  for (auto &worker : impl_->workers) {
    if (worker->thread.joinable())
      worker->thread.join();
  }
}

void Runtime::shutdown()
{
  request_stop();
  join();
}

} // namespace weave
