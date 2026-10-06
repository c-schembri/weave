#include <weave/runtime.hpp>
#include <weave/io/detail/trace.hpp>
#include <algorithm>
#include <mutex>
#include <array>
#include <thread>
#include <vector>

namespace weave {

namespace {

thread_local detail::RuntimeTask *current_task = nullptr;
thread_local void *current_scheduler = nullptr;
thread_local std::size_t current_worker = 0;

} // namespace

struct Runtime::Impl {
  struct ReadyQueue {
    detail::RuntimeTask *first = nullptr;
    detail::RuntimeTask *last = nullptr;
    std::size_t size = 0;

    bool empty() const noexcept
    {
      return first == nullptr;
    }

    void push_back(detail::RuntimeTask &task) noexcept
    {
      task.queue_next = nullptr;
      task.queue_previous = last;
      if (last)
        last->queue_next = &task;
      else
        first = &task;
      last = &task;
      ++size;
    }

    detail::RuntimeTask *pop(bool back = false) noexcept
    {
      auto *task = back ? last : first;
      if (!task)
        return nullptr;
      if (task->queue_previous)
        task->queue_previous->queue_next = task->queue_next;
      else
        first = task->queue_next;
      if (task->queue_next)
        task->queue_next->queue_previous = task->queue_previous;
      else
        last = task->queue_previous;
      task->queue_next = nullptr;
      task->queue_previous = nullptr;
      --size;
      return task;
    }
  };

  struct alignas(64) Worker {
    std::thread thread;
    Context *context = nullptr;
    Error error;
    std::atomic<bool> ready{false};
    std::atomic<bool> idle{false};
    std::atomic<bool> dispatching{false};
    std::mutex queue_mutex;
    ReadyQueue movable;
    ReadyQueue pinned;
    bool pinned_turn = true;
    std::size_t next_victim = 0;
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
    if (active.load(std::memory_order_acquire) != 0 || dispatching.load(std::memory_order_acquire) != 0)
      return false;
    for (const auto &worker : workers) {
      if (worker->dispatching.load(std::memory_order_acquire))
        return false;
    }
    return true;
  }

  void enqueue(detail::RuntimeTask &task)
  {
    const auto index = task.worker;
    const bool pinned = task.pinned;
    auto &worker = *workers[index];
    detail::trace(detail::TraceEvent::enqueue, &task, index);

    {
      std::lock_guard lock(worker.queue_mutex);
      (pinned ? worker.pinned : worker.movable).push_back(task);
    }

    // Marking idle before rechecking queues closes the enqueue/park lost-wake race.
    const bool waking_owner = worker.idle.exchange(false, std::memory_order_acq_rel);
    if (waking_owner)
      detail::ContextAccess::wake(*worker.context);

    if (!pinned && !waking_owner)
      wake_thief(index);
  }

  void wake_thief(std::size_t index) noexcept
  {
    for (std::size_t n = 1; n < workers.size(); ++n) {
      auto &thief = *workers[(index + n) % workers.size()];
      if (thief.idle.load(std::memory_order_relaxed) && thief.idle.exchange(false, std::memory_order_acq_rel)) {
        detail::ContextAccess::wake(*thief.context);
        break;
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
        auto *task = queue.pop();
        worker.pinned_turn = !worker.pinned_turn;
        return task;
      }
    }

    const auto start = worker.next_victim++ % workers.size();
    for (std::size_t n = 0; n < workers.size(); ++n) {
      const auto victim_index = (start + n) % workers.size();
      if (victim_index == index)
        continue;
      auto &victim = *workers[victim_index];
      std::array<detail::RuntimeTask *, 16> stolen{};
      std::size_t count = 0;
      {
        std::lock_guard lock(victim.queue_mutex);
        if (victim.movable.empty())
          continue;
        count = (std::min)(stolen.size(), (victim.movable.size + 1) / 2);
        for (std::size_t i = 0; i < count; ++i)
          stolen[i] = victim.movable.pop(true);
      }
      // Never hold a victim and destination queue lock together.
      if (count > 1) {
        {
          std::lock_guard lock(worker.queue_mutex);
          for (std::size_t i = 1; i < count; ++i)
            worker.movable.push_back(*stolen[i]);
        }
        wake_thief(index);
      }
      detail::trace(detail::TraceEvent::steal, stolen[0], count);
      return stolen[0];
    }

    return nullptr;
  }

  void execute(detail::RuntimeTask &task, std::size_t worker)
  {
    auto &execution = *workers[worker];
    execution.dispatching.store(true, std::memory_order_release);
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
    detail::trace(detail::TraceEvent::execute_begin, &task, reinterpret_cast<std::uintptr_t>(event->state));
    current_task = &task;
    detail::current_executor = &task;
    const auto invoke = event->invoke;
    auto *state = event->state;
    invoke(state);
    detail::current_executor = nullptr;
    current_task = nullptr;
    detail::trace(detail::TraceEvent::execute_end, &task);

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
    execution.dispatching.store(false, std::memory_order_release);
    if (closing.load(std::memory_order_acquire) && active.load(std::memory_order_acquire) == 0) {
      std::lock_guard lock(submissions);
      wake_locked();
    }
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
        [](void *state) noexcept {
          static_cast<Impl *>(state)->active.fetch_add(1, std::memory_order_relaxed);
        },
        [](void *state) noexcept {
          static_cast<Impl *>(state)->finished();
        }});
    detail::ContextAccess::enter(*ctx, scheduler == Scheduler::work_stealing ? this : nullptr);
    current_scheduler = this;
    current_worker = index;
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
        detail::trace(detail::TraceEvent::park_begin, &worker, index);
        detail::ContextAccess::poll(*ctx);
        detail::trace(detail::TraceEvent::park_end, &worker, index);
      }
      worker.idle.store(false, std::memory_order_release);
    }
    {
      // Exclude late wake-ups before the worker destroys its native I/O backend.
      std::lock_guard lock(submissions);
      worker.context = nullptr;
    }
    detail::ContextAccess::leave(*ctx);
    current_scheduler = nullptr;
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
        [](void *state) noexcept {
          static_cast<Impl *>(state)->dispatching.fetch_add(1, std::memory_order_acq_rel);
        },
        [](void *state) noexcept {
          static_cast<Impl *>(state)->dispatch_finished();
        }});
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
    worker.thread = std::thread([state = impl.get(), i, options] {
      state->run(i, options.context);
    });
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
  if (impl_->scheduler == Scheduler::work_stealing) {
    task.event.executor = &task;
    task.dispatch_completion = detail::dispatch_completion;
  }

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

void detail::dispatch_completion(Posted &message) noexcept
{
  auto &task = *static_cast<RuntimeTask *>(message.executor);
  auto *runtime = static_cast<Runtime *>(task.owner);
  const bool shared = runtime->impl_->io_layout == IoLayout::shared;
  if (shared && current_scheduler == runtime->impl_.get() && !current_task && !current_executor) {
    std::unique_lock lock(task.ready_mutex);
    if (!task.scheduled && (!task.pinned || task.worker == current_worker)) {
      // Only native completions take this path. Submission and frame cleanup stay deferred.
      require(task.first == nullptr && task.last == nullptr);
      task.scheduled = true;
      message.next = nullptr;
      task.first = task.last = &message;
      lock.unlock();
      trace(TraceEvent::local_completion, &task, current_worker);
      runtime->impl_->execute(task, current_worker);
      return;
    }
  }
  schedule(message);
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
