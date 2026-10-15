#pragma once

#include <weave/core.hpp>
#include <weave/io/detail/execution.hpp>
#include <weave/io/detail/spawn.hpp>
#include <memory>

namespace weave {

namespace detail {

struct ContextAccess;
struct IoAccess;

} // namespace detail

/// Backend policy for one Context; not a worker count or a thread-safety hint.
struct ContextOptions {
  /// Allow inline native completion where supported; Linux tries nonblocking sends before io_uring.
  bool skip_successful_completions = true;
};

/// Single-owner event loop driven on the calling thread. Does not create worker threads.
/// Keep its stable address alive until sockets, borrowed operations and submitted work have drained.
/// run/shutdown require the owner; spawn/detach and request_stop permit cross-thread submission.
class Context {
  struct Impl;

  class CreateKey {
    friend struct detail::IoAccess;
    CreateKey() = default;
  };

public:
  /// Create checked backend resources without running the loop. Returns an immovable Result<Context>.
  /// Initialization failures are error codes; keep the owning Result alive rather than moving out the Context.
  [[nodiscard]] static Result<Context> create(ContextOptions options = {}) noexcept;

  // Only the factory can supply this key; public for Result's in-place construction.
  Context(CreateKey, std::unique_ptr<Impl> impl) noexcept;
  /// Cancel and drain owned work before releasing backend resources; cooperative cleanup may block.
  ~Context();
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  /// Whether shutdown/stop has been requested; not proof that queued work or I/O has drained.
  bool stop_requested() const noexcept;

  /// Thread-safe submission transferring an unstarted Task to this Context and returning its JoinHandle.
  /// Independent work uses options.cancel, not implicit parent cancellation; borrowed task data remains caller-owned.
  template <class T>
  Result<JoinHandle<T>> spawn(Task<T> operation, SpawnOptions options = {});
  /// Own a copied/moved factory through completion and construct its Task on the owner thread.
  /// Factories may take Context& or no arguments. Rejection destroys the unstarted factory on the submitter.
  template <detail::SpawnFactory F>
  auto spawn(F &&factory, SpawnOptions options = {}) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>;

  /// Transfer independent Task ownership without a join handle. Rejection invokes on_error on the submitter;
  /// execution errors invoke it on the owner after cleanup. Omitted handlers discard errors; captures may still borrow.
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach(Task<T> operation, H on_error = {});
  /// Detached Task submission with explicit cancellation; otherwise identical to detach(operation, on_error).
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach(Task<T> operation, SpawnOptions options, H on_error = {});
  /// Own a factory through detached completion; construct/run its Task on the owner, not on the submitter.
  /// Captured references must outlive the work; noexcept observer timing matches Task detach.
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach(F &&factory, H on_error = {});
  /// Detached factory submission with explicit cancellation; copies lvalue factories and moves rvalues.
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach(F &&factory, SpawnOptions options, H on_error = {});

  /// Run submitted work on the calling owner thread until request_stop(), then cancel/drain owned work.
  /// Establishes the active Context for contextless I/O; do not nest run calls.
  void run();
  /// Thread-safe stop request and wake-up; does not wait for completion.
  void request_stop() noexcept;
  /// Stop admission, request cooperative cancellation and drive owner-thread cleanup until owned work drains.
  void shutdown();

  /// Scheduling awaiter from yield(); retain the referenced Context through the await.
  struct Yield {
    Context &context;
    detail::Posted message{};

    bool await_ready() const noexcept
    {
      return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) noexcept;

    void await_resume() const noexcept
    {
    }
  };

  /// co_await to reschedule the current continuation through this Context; does not create independent work.
  /// Requires compatible active execution and borrows the Context through the await.
  [[nodiscard]] Yield yield() noexcept
  {
    return Yield{*this};
  }

  /// Consume a Task and drive the owner-thread event loop until it finishes, returning Result<T>.
  /// Does not join independent spawned/detached tasks or close admission; join borrowers before their data dies.
  template <class T>
  Result<T> run(Task<T> operation)
  {
    enter(nullptr);
    detail::TaskAccess::inherit(operation, detail::context_cancellation(*this));
    detail::TaskAccess::start(operation);
    while (!detail::TaskAccess::done(operation))
      poll();

    auto result = detail::TaskAccess::take(operation);
    leave();
    return result;
  }

  /// Snapshot of this Context's diagnostic counters; profiling-only timing counters are not production benchmarks.
  struct Metrics {
    u64 submitted = 0;
    u64 completed = 0;
    u64 dequeue_calls = 0;
    u64 inline_completions = 0;
    u64 fairness_posts = 0;
    u64 read_calls = 0;
    u64 write_calls = 0;
    u64 read_bytes = 0;
    u64 write_bytes = 0;
    u64 immediate_successes = 0;
    u64 submission_ns = 0;
    u64 read_submission_ns = 0;
    u64 write_submission_ns = 0;
    u64 dequeue_ns = 0;
  };

  /// Read counters on the Context's owner thread; does not synchronize unrelated worker activity.
  Metrics metrics() const noexcept;

private:
  friend struct detail::ContextAccess;
  friend struct detail::IoAccess;
  std::unique_ptr<Impl> impl_;
  void check_thread() const noexcept;
  void enter(void *scheduler_group) noexcept;
  void leave() noexcept;
  void post(detail::Posted &message) noexcept;
  void cancel_pending() noexcept;
  void poll(bool wait = true);
  void count(u64 &counter, u64 amount = 1) noexcept;
  Result<void> submit(detail::SpawnBase &task);
  void finished() noexcept;
  void close_submissions() noexcept;
  void observe(detail::TaskObserver observer) noexcept;
};

template <class T>
Result<JoinHandle<T>> Context::spawn(Task<T> operation, SpawnOptions options)
{
  return spawn(detail::TaskSubmission<T>{std::move(operation)}, options);
}

template <detail::SpawnFactory F>
auto Context::spawn(F &&factory, SpawnOptions options) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>
{
  using Function = std::decay_t<F>;
  auto *task = new (std::nothrow) detail::SpawnTask<Function, detail::SpawnBase>(
    Function(std::forward<F>(factory)),
    detail::SpawnMode::joinable,
    {},
    options);
  detail::require(task != nullptr);

  auto accepted = submit(*task);
  if (!accepted) {
    delete task;
    return std::unexpected(accepted.error());
  }
  return task->handle();
}

template <class T, detail::ErrorObserver H>
void Context::detach(Task<T> operation, H on_error)
{
  detach(std::move(operation), SpawnOptions{}, std::move(on_error));
}

template <class T, detail::ErrorObserver H>
void Context::detach(Task<T> operation, SpawnOptions options, H on_error)
{
  detach(detail::TaskSubmission<T>{std::move(operation)}, options, std::move(on_error));
}

template <detail::SpawnFactory F, detail::ErrorObserver H>
void Context::detach(F &&factory, H on_error)
{
  detach(std::forward<F>(factory), SpawnOptions{}, std::move(on_error));
}

template <detail::SpawnFactory F, detail::ErrorObserver H>
void Context::detach(F &&factory, SpawnOptions options, H on_error)
{
  using Function = std::decay_t<F>;
  auto *task = new (std::nothrow) detail::SpawnTask<Function, detail::SpawnBase, H>(
    Function(std::forward<F>(factory)),
    detail::SpawnMode::detached,
    std::move(on_error),
    options);
  detail::require(task != nullptr);

  auto accepted = submit(*task);
  if (!accepted) {
    task->report_error(accepted.error());
    delete task;
  }
}

} // namespace weave
