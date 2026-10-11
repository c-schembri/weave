#pragma once

#include <weave/io.hpp>
#include <weave/io/detail/context_access.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <type_traits>

namespace weave {

class Runtime;

namespace detail {

using RuntimeTask = ScheduledSpawn;

void schedule(Posted &message) noexcept;
void dispatch_completion(Posted &message) noexcept;

} // namespace detail

/// Root placement policy: keep tasks on a worker, or allow independent roots to migrate.
enum class Scheduler {
  worker_affine,
  work_stealing
};

/// Native completion layout. shared is Windows-only; scheduling policy is independent.
enum class IoLayout {
  sharded,
  shared
};

/// Worker count, scheduling and native I/O configuration copied during create().
struct RuntimeOptions {
  std::size_t workers = 0; // Zero uses hardware_concurrency(), or one if unknown.
  Scheduler scheduler = Scheduler::worker_affine;
  ContextOptions context{};
  IoLayout io_layout = IoLayout::sharded;
};

/// Owns worker threads and their Contexts. Submission is thread-safe; lifetime is caller-owned.
/// Keep this immovable object alive until tasks, sockets and other borrowers are destroyed.
class Runtime {
  struct Impl;

  class CreateKey {
    friend class Runtime;
    CreateKey() = default;
  };

public:
  /// Start workers or return a setup error after stopping any partially started workers.
  /// The returned Result is immovable. workers == 0 selects logical hardware concurrency.
  [[nodiscard]] static Result<Runtime> create(RuntimeOptions options = {}) noexcept;

  // Only the factory can supply this key; public for Result's in-place construction.
  Runtime(CreateKey, std::unique_ptr<Impl> impl) noexcept;
  /// Cooperatively cancel accepted work and join workers. Uncooperative work can delay destruction.
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  /// Number of worker threads successfully created.
  std::size_t worker_count() const noexcept;
  /// Configured task placement policy.
  Scheduler scheduler() const noexcept;
  /// Configured native completion layout.
  IoLayout io_layout() const noexcept;
  /// Whether cancelling stop/shutdown was requested; join() alone closes admission without setting this.
  bool stop_requested() const noexcept;
  /// Thread-safe request to stop admission, request worker I/O cancellation and wake workers; no join.
  void request_stop() noexcept;
  /// Close admission and block until accepted work finishes, without cancelling it.
  /// Call outside running Contexts; an unbounded server task can prevent return.
  void join();
  /// Request cancellation and block until workers drain. Call outside running Contexts.
  void shutdown();

  /// Consume a lazy task and block this caller for its Result, not for independently spawned work.
  /// Must be called outside a running Context. Borrowed arguments must survive completion.
  template <class T>
  Result<T> run(Task<T> operation);
  /// Own a factory, invoke it on a worker with Context& or no arguments, then wait for its Result.
  /// The closure survives completion; references captured by it remain caller-owned.
  template <detail::SpawnFactory F>
  auto run(F &&factory) -> Result<detail::SpawnResult<std::decay_t<F>>>;

  /// Thread-safe independent submission. Owns the task frame, not its referenced arguments.
  /// Cancellation is explicit via options; dropping the handle neither cancels nor joins.
  template <class T>
  Result<JoinHandle<T>> spawn(Task<T> operation, SpawnOptions options = {});
  /// Own the factory through completion and construct its task on a worker.
  /// Rejection returns an error synchronously; captured references must outlive the task.
  template <detail::SpawnFactory F>
  auto spawn(F &&factory, SpawnOptions options = {}) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>;
  /// Submit an independent task pinned to a worker index; invalid indices are rejected.
  template <class T>
  Result<JoinHandle<T>> spawn_on(std::size_t worker, Task<T> operation, SpawnOptions options = {});
  /// Own a factory and pin its invocation and task to the selected worker.
  template <detail::SpawnFactory F>
  auto spawn_on(std::size_t worker, F &&factory, SpawnOptions options = {})
    -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>;

  /// Own unjoined work. The noexcept observer sees rejection on the caller or failure on a worker
  /// after task cleanup. Without an observer, errors are discarded; references are not owned.
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach(Task<T> operation, H on_error = {});
  /// Detach an owned task with explicit cancellation; no join handle is created.
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach(Task<T> operation, SpawnOptions options, H on_error = {});
  /// Own a factory through its detached task; invoke it on a worker, not the submitter.
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach(F &&factory, H on_error = {});
  /// Detach an owned factory with explicit cancellation and an optional noexcept observer.
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach(F &&factory, SpawnOptions options, H on_error = {});
  /// Detach an owned task pinned to a worker; invalid indices reach the observer synchronously.
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach_on(std::size_t worker, Task<T> operation, H on_error = {});
  /// Pin detached work and use the supplied cancellation token; does not join.
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach_on(std::size_t worker, Task<T> operation, SpawnOptions options, H on_error = {});
  /// Own a factory and pin its invocation and detached task to the selected worker.
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach_on(std::size_t worker, F &&factory, H on_error = {});
  /// Pin an owned detached factory with explicit cancellation and optional error observation.
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach_on(std::size_t worker, F &&factory, SpawnOptions options, H on_error = {});

private:
  friend void detail::schedule(detail::Posted &) noexcept;
  friend void detail::dispatch_completion(detail::Posted &) noexcept;
  std::unique_ptr<Impl> impl_;
  Result<void> submit(
    detail::RuntimeTask &task,
    std::size_t worker,
    std::size_t local_worker = static_cast<std::size_t>(-1));
  void finished() noexcept;
  static void complete(detail::SpawnBase *task) noexcept;
};

template <class T>
Result<T> Runtime::run(Task<T> operation)
{
  detail::require(!detail::current_context);
  auto job = spawn(std::move(operation));
  if (!job)
    return std::unexpected(job.error());
  return std::move(*job).get();
}

template <detail::SpawnFactory F>
auto Runtime::run(F &&factory) -> Result<detail::SpawnResult<std::decay_t<F>>>
{
  detail::require(!detail::current_context);
  auto job = spawn(std::forward<F>(factory));
  if (!job)
    return std::unexpected(job.error());
  return std::move(*job).get();
}

template <class T>
Result<JoinHandle<T>> Runtime::spawn(Task<T> operation, SpawnOptions options)
{
  return spawn_on(static_cast<std::size_t>(-1), std::move(operation), options);
}

template <detail::SpawnFactory F>
auto Runtime::spawn(F &&factory, SpawnOptions options) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>
{
  return spawn_on(static_cast<std::size_t>(-1), std::forward<F>(factory), options);
}

template <class T>
Result<JoinHandle<T>> Runtime::spawn_on(std::size_t worker, Task<T> operation, SpawnOptions options)
{
  return spawn_on(worker, detail::TaskSubmission<T>{std::move(operation)}, options);
}

template <detail::SpawnFactory F>
auto Runtime::spawn_on(std::size_t worker, F &&factory, SpawnOptions options)
  -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>
{
  using Function = std::decay_t<F>;

  auto *task = new (std::nothrow) detail::SpawnTask<Function, detail::RuntimeTask>(
    Function(std::forward<F>(factory)),
    detail::SpawnMode::joinable,
    {},
    options);
  detail::require(task != nullptr);

  auto accepted = submit(*task, worker);
  if (!accepted) {
    delete task;
    return std::unexpected(accepted.error());
  }

  return task->handle();
}

template <class T, detail::ErrorObserver H>
void Runtime::detach(Task<T> operation, H on_error)
{
  detach(std::move(operation), SpawnOptions{}, std::move(on_error));
}

template <class T, detail::ErrorObserver H>
void Runtime::detach(Task<T> operation, SpawnOptions options, H on_error)
{
  detach_on(static_cast<std::size_t>(-1), std::move(operation), options, std::move(on_error));
}

template <detail::SpawnFactory F, detail::ErrorObserver H>
void Runtime::detach(F &&factory, H on_error)
{
  detach(std::forward<F>(factory), SpawnOptions{}, std::move(on_error));
}

template <detail::SpawnFactory F, detail::ErrorObserver H>
void Runtime::detach(F &&factory, SpawnOptions options, H on_error)
{
  detach_on(static_cast<std::size_t>(-1), std::forward<F>(factory), options, std::move(on_error));
}

template <class T, detail::ErrorObserver H>
void Runtime::detach_on(std::size_t worker, Task<T> operation, H on_error)
{
  detach_on(worker, std::move(operation), SpawnOptions{}, std::move(on_error));
}

template <class T, detail::ErrorObserver H>
void Runtime::detach_on(std::size_t worker, Task<T> operation, SpawnOptions options, H on_error)
{
  detach_on(worker, detail::TaskSubmission<T>{std::move(operation)}, options, std::move(on_error));
}

template <detail::SpawnFactory F, detail::ErrorObserver H>
void Runtime::detach_on(std::size_t worker, F &&factory, H on_error)
{
  detach_on(worker, std::forward<F>(factory), SpawnOptions{}, std::move(on_error));
}

template <detail::SpawnFactory F, detail::ErrorObserver H>
void Runtime::detach_on(std::size_t worker, F &&factory, SpawnOptions options, H on_error)
{
  using Function = std::decay_t<F>;
  auto *task = new (std::nothrow) detail::SpawnTask<Function, detail::RuntimeTask, H>(
    Function(std::forward<F>(factory)),
    detail::SpawnMode::detached,
    std::move(on_error),
    options);
  detail::require(task != nullptr);

  auto accepted = submit(*task, worker);
  if (!accepted) {
    task->report_error(accepted.error());
    delete task;
  }
}

} // namespace weave
