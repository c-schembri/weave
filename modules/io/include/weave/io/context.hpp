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

struct ContextOptions {
  bool skip_successful_completions = true;
};

class Context {
  struct Impl;

  class CreateKey {
    friend struct detail::IoAccess;
    CreateKey() = default;
  };

public:
  [[nodiscard]] static Result<Context> create(ContextOptions options = {}) noexcept;

  // Only the factory can supply this key; public for Result's in-place construction.
  Context(CreateKey, std::unique_ptr<Impl> impl) noexcept;
  ~Context();
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  bool stop_requested() const noexcept;

  // Thread-safe submission. Factories run on the owner; prebuilt tasks transfer frame ownership.
  template <class T>
  Result<JoinHandle<T>> spawn(Task<T> operation, SpawnOptions options = {});
  template <detail::SpawnFactory F>
  auto spawn(F &&factory, SpawnOptions options = {}) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>;

  // Rejection is reported on the caller; task errors on the owner. Omitted handlers discard errors.
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach(Task<T> operation, H on_error = {});
  template <class T, detail::ErrorObserver H = detail::IgnoreError>
  void detach(Task<T> operation, SpawnOptions options, H on_error = {});
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach(F &&factory, H on_error = {});
  template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
  void detach(F &&factory, SpawnOptions options, H on_error = {});

  // Serve submissions on the calling thread until stopped, then drain owned tasks.
  void run();
  void request_stop() noexcept;
  void shutdown();

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

  [[nodiscard]] Yield yield() noexcept
  {
    return Yield{*this};
  }

  // Drive this thread's event loop until the task completes.
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
