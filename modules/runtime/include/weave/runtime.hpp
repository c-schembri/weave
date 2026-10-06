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

enum class Scheduler {
  worker_affine,
  work_stealing
};

enum class IoLayout {
  sharded,
  shared
};

struct RuntimeOptions {
  std::size_t workers = 0; // Zero uses hardware_concurrency(), or one if unknown.
  Scheduler scheduler = Scheduler::worker_affine;
  ContextOptions context{};
  IoLayout io_layout = IoLayout::sharded;
};

class Runtime {
  struct Impl;

  class CreateKey {
    friend class Runtime;
    CreateKey() = default;
  };

public:
  [[nodiscard]] static Result<Runtime> create(RuntimeOptions options = {}) noexcept;

  // Only the factory can supply this key; public for Result's in-place construction.
  Runtime(CreateKey, std::unique_ptr<Impl> impl) noexcept;
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  std::size_t worker_count() const noexcept;
  Scheduler scheduler() const noexcept;
  IoLayout io_layout() const noexcept;
  bool stop_requested() const noexcept;
  void request_stop() noexcept;
  void join();     // Close submissions and drain accepted work without cancelling it.
  void shutdown(); // Request I/O cancellation, then join all workers.

  // Schedule a root and wait on the caller, without closing submissions or joining independent tasks.
  template <class T>
  Result<T> run(Task<T> operation);
  template <detail::SpawnFactory F>
  auto run(F &&factory) -> Result<detail::SpawnResult<std::decay_t<F>>>;

  template <class T>
  Result<JoinHandle<T>> spawn(Task<T> operation, SpawnOptions options = {});
  template <detail::SpawnFactory F>
  auto spawn(F &&factory, SpawnOptions options = {}) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>;
  template <class T>
  Result<JoinHandle<T>> spawn_on(std::size_t worker, Task<T> operation, SpawnOptions options = {});
  template <detail::SpawnFactory F>
  auto spawn_on(std::size_t worker, F &&factory, SpawnOptions options = {})
    -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>;

  // Rejection is reported on the caller; task errors on a worker. Omitted handlers discard errors.
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach(Task<T> operation, H on_error = {});
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach(Task<T> operation, SpawnOptions options, H on_error = {});
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach(F &&factory, H on_error = {});
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach(F &&factory, SpawnOptions options, H on_error = {});
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach_on(std::size_t worker, Task<T> operation, H on_error = {});
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach_on(std::size_t worker, Task<T> operation, SpawnOptions options, H on_error = {});
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach_on(std::size_t worker, F &&factory, H on_error = {});
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
