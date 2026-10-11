#pragma once

#include <weave/core.hpp>
#include <weave/io/detail/execution.hpp>
#include <atomic>

namespace weave {

namespace detail {

template <class F, class Base, class H>
struct SpawnTask;

inline char join_completed;

struct JoinStateBase {
  std::atomic<unsigned> references{2}; // One execution owner and one result handle.
  std::atomic<void *> completion{nullptr};
  void (*destroy)(JoinStateBase *) noexcept = nullptr;
  CancelSource cancellation;
  std::optional<CancelLink> group_link;
  std::optional<CancelLink> context_link;

  void release() noexcept
  {
    if (references.fetch_sub(1, std::memory_order_acq_rel) == 1)
      destroy(this);
  }

  bool ready() const noexcept
  {
    return completion.load(std::memory_order_acquire) == &join_completed;
  }

  void publish() noexcept
  {
    auto *waiting = completion.exchange(&join_completed, std::memory_order_acq_rel);
    require(waiting != &join_completed);
    completion.notify_all();

    if (waiting) {
      auto &message = *static_cast<Posted *>(waiting);
      post(*message.target, message);
    }
  }
};

template <class T>
struct JoinState : JoinStateBase {
  std::optional<Result<T>> result;

  Result<T> take() noexcept
  {
    require(result.has_value());
    return std::move(*result);
  }
};

} // namespace detail

/// Move-only result handle for independently owned work. Dropping it neither cancels nor joins that work.
/// Await/get consume it once; the executor retains the task through completion even if the handle is dropped.
template <class T>
class [[nodiscard]] JoinHandle {
  detail::JoinState<T> *state_;
  template <class F, class Base, class H>
  friend struct detail::SpawnTask;

  explicit JoinHandle(detail::JoinState<T> *state) noexcept : state_(state)
  {
  }

public:
  JoinHandle(JoinHandle &&other) noexcept : state_(std::exchange(other.state_, nullptr))
  {
  }

  JoinHandle &operator=(JoinHandle &&other) noexcept
  {
    if (this != &other) {
      if (state_)
        state_->release();
      state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
  }

  JoinHandle(const JoinHandle &) = delete;

  ~JoinHandle()
  {
    if (state_)
      state_->release();
  }

  /// Nonblocking, thread-safe completion observation; does not consume the result.
  bool ready() const noexcept
  {
    detail::require(state_ != nullptr);
    return state_->ready();
  }

  /// Request cooperative cancellation without waiting; get/co_await still drains and observes the outcome.
  void cancel() noexcept
  {
    detail::require(state_ != nullptr);
    state_->cancellation.cancel();
  }

  /// Consume the handle and block for Result<T>. Forbidden inside a running Context; await there instead.
  /// An executor must drive the task elsewhere; this call does not run a Context event loop.
  Result<T> get() &&
  {
    detail::require(state_ && !detail::current_context);
    auto *state = std::exchange(state_, nullptr);

    while (!state->ready())
      state->completion.wait(nullptr, std::memory_order_acquire);

    auto result = state->take();
    state->release();
    return result;
  }

  struct Awaiter {
    detail::JoinState<T> *state;
    detail::Posted message{};

    explicit Awaiter(detail::JoinState<T> *s) noexcept : state(s)
    {
    }

    Awaiter(const Awaiter &) = delete;

    ~Awaiter()
    {
      state->release();
    }

    bool await_ready() const noexcept
    {
      return state->ready();
    }

    bool await_suspend(std::coroutine_handle<> continuation) noexcept
    {
      detail::require(detail::current_context != nullptr);

      message.target = detail::current_context;
      message.executor = detail::current_executor;
      message.state = continuation.address();
      message.invoke = [](void *address) noexcept {
        std::coroutine_handle<>::from_address(address).resume();
      };

      void *expected = nullptr;
      auto registered = state->completion.compare_exchange_strong(expected, &message, std::memory_order_acq_rel);
      if (registered)
        return true;

      detail::require(expected == &detail::join_completed);
      return false;
    }

    Result<T> await_resume()
    {
      detail::require(state->ready());
      return state->take();
    }
  };

private:
  static Task<T> wait(JoinHandle handle)
  {
    auto result = co_await Awaiter{std::exchange(handle.state_, nullptr)};
    if (!result)
      co_await fail(result.error());
    if constexpr (!std::is_void_v<T>)
      co_return std::move(*result);
  }

public:
  /// Consume the handle into a lazy join Task; failure propagates normally when awaited.
  Task<T> as_task() && noexcept
  {
    detail::require(state_ != nullptr);
    return wait(std::move(*this));
  }

  /// Consume and join from active Weave execution, yielding T or propagating failure to the enclosing Task.
  auto operator co_await() && noexcept
  {
    return detail::TaskAwaiter<T, false>{std::move(*this).as_task()};
  }
};

/// Consume and join a handle, capturing Result<T> for recovery rather than propagating the child's failure.
template <class T>
auto as_result(JoinHandle<T> handle) noexcept
{
  return as_result(std::move(handle).as_task());
}

} // namespace weave
