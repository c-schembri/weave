#pragma once

#include <weave/io/detail/context_access.hpp>
#include <weave/io/detail/scheduled_spawn.hpp>
#include <mutex>

namespace weave {

class TaskScope {
  CancelSource cancellation_;
  std::mutex mutex_;
  std::size_t active_ = 0;
  std::optional<Error> error_;
  detail::Posted *waiting_ = nullptr;
  bool closing_ = false;

  static void finished(void *state, Result<void> result) noexcept
  {
    auto &scope = *static_cast<TaskScope *>(state);
    detail::Posted *waiting = nullptr;
    {
      std::lock_guard lock(scope.mutex_);
      detail::require(scope.active_ != 0);
      if (!scope.error_ && !result)
        scope.error_.emplace(result.error());
      if (--scope.active_ == 0)
        waiting = std::exchange(scope.waiting_, nullptr);
    }
    if (waiting)
      detail::post(*waiting->target, *waiting);
  }

  struct Drain {
    TaskScope &scope;
    detail::Posted event{};

    bool await_ready() const noexcept
    {
      return false;
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept
    {
      detail::require(detail::current_context != nullptr);
      event.target = detail::current_context;
      event.executor = detail::current_executor;
      event.state = continuation.address();
      event.invoke = [](void *address) noexcept { std::coroutine_handle<>::from_address(address).resume(); };
      std::lock_guard lock(scope.mutex_);
      scope.closing_ = true;
      detail::require(scope.waiting_ == nullptr);
      if (scope.active_ == 0)
        return false;
      scope.waiting_ = &event;
      return true;
    }

    Result<void> await_resume() noexcept
    {
      std::lock_guard lock(scope.mutex_);
      detail::require(scope.active_ == 0);
      if (scope.error_)
        return std::unexpected(*scope.error_);
      return {};
    }
  };

  static Task<void> drain(TaskScope &scope)
  {
    auto result = co_await Drain{scope};
    if (!result)
      co_await fail(result.error());
  }

public:
  TaskScope() = default;
  TaskScope(const TaskScope &) = delete;

  ~TaskScope()
  {
    detail::require(active_ == 0);
  }

  CancelToken token() const noexcept
  {
    return cancellation_.token();
  }

  void cancel() noexcept
  {
    cancellation_.cancel();
  }

  // Closing join is shielded: cancellation must never bypass draining children.
  Task<void> join()
  {
    auto task = drain(*this);
    detail::TaskAccess::bind(task, {});
    return task;
  }

  template <detail::SpawnFactory F>
  auto spawn(F &&factory) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>
  {
    detail::require(detail::current_context != nullptr);
    {
      std::lock_guard lock(mutex_);
      detail::require(!closing_);
      ++active_;
    }
    using Function = std::decay_t<F>;
    const auto *submission = detail::current_submission;
    if (submission) {
      auto *task = new (std::nothrow) detail::SpawnTask<Function, detail::ScheduledSpawn>(
        Function(std::forward<F>(factory)),
        detail::SpawnMode::joinable,
        {},
        SpawnOptions{token()});
      detail::require(task != nullptr);
      task->child_observer = {this, finished};
      auto accepted = submission->submit(submission->state, submission->worker, *task);
      if (!accepted) {
        delete task;
        finished(this, std::unexpected(accepted.error()));
        return std::unexpected(accepted.error());
      }
      return task->handle();
    }
    auto *task = new (std::nothrow) detail::SpawnTask<Function, detail::SpawnBase>(
      Function(std::forward<F>(factory)),
      detail::SpawnMode::joinable,
      {},
      SpawnOptions{token()});
    detail::require(task != nullptr);
    task->child_observer = {this, finished};
    auto accepted = detail::ContextAccess::submit(*detail::current_context, *task);
    if (!accepted) {
      delete task;
      finished(this, std::unexpected(accepted.error()));
      return std::unexpected(accepted.error());
    }
    return task->handle();
  }

  template <class T>
  Result<JoinHandle<T>> spawn(Task<T> task)
  {
    return spawn(detail::TaskSubmission<T>{std::move(task)});
  }
};

namespace detail {

// Recovery without reclamation: a failed body's locals can still be borrowed by children.
template <class T>
struct RetainedTask {
  Task<T> &task;
  std::coroutine_handle<> continuation;
  bool armed = false;

  bool await_ready() const noexcept
  {
    return false;
  }

  static std::coroutine_handle<> completed(void *state) noexcept
  {
    auto &self = *static_cast<RetainedTask *>(state);
    return self.armed ? self.continuation : std::noop_coroutine();
  }

  bool await_suspend(std::coroutine_handle<> parent) noexcept
  {
    continuation = parent;
    TaskAccess::start(task, this, completed);
    armed = true;
    return !TaskAccess::done(task);
  }

  void await_resume() const noexcept
  {
  }
};

} // namespace detail

template <class F>
  requires std::invocable<F &, TaskScope &> && std::same_as<std::invoke_result_t<F &, TaskScope &>, Task<void>>
Task<void> scope(F body)
{
  TaskScope children;
  auto parent = co_await detail::GetCancellation{};

  // Forward cancellation into the same source used by every scope child.
  struct CancelChildren {
    TaskScope *scope;

    void operator()() const noexcept
    {
      scope->cancel();
    }
  };

  std::stop_callback callback(parent.native_token(), CancelChildren{&children});
  auto operation = std::invoke(body, children);
  detail::TaskAccess::bind(operation, children.token());
  co_await detail::RetainedTask<void>{operation};
  // Retain the body frame until children drain, including its failed await chain.
  auto body_failed = detail::TaskAccess::failed(operation);
  if (body_failed)
    children.cancel();
  auto drained = co_await as_result(children.join());
  auto result = detail::TaskAccess::take(operation);
  if (!result)
    co_await fail(result.error());
  if (!drained)
    co_await fail(drained.error());
}

} // namespace weave
