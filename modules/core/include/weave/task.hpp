#pragma once

#include <weave/core/detail/frame_allocator.hpp>
#include <weave/cancellation.hpp>
#include <array>
#include <coroutine>
#include <cstdlib>
#include <optional>
#include <utility>
#include <concepts>
#include <cstdio>
#include <expected>
#include <functional>
#include <source_location>
#include <system_error>
#include <type_traits>

namespace weave {

using Error = std::error_code;
template <class T>
using Result = std::expected<T, Error>;
template <class T = void>
class Task;

namespace detail {

inline void require(bool condition, std::source_location location = std::source_location::current()) noexcept
{
  if (!condition) {
    std::fprintf(stderr, "Weave contract: %s:%u\n", location.file_name(), location.line());
    std::abort();
  }
}

template <class F>
concept ErrorObserver = std::is_nothrow_move_constructible_v<F> && requires(F &observer, const Error &error) {
  { std::invoke(observer, error) } noexcept -> std::same_as<void>;
};

struct Owner;
struct TaskAccess;
enum class State {
  created,
  running,
  succeeded,
  failed
};

struct PromiseBase {
  std::coroutine_handle<> self;
  std::coroutine_handle<> continuation = std::noop_coroutine();
  PromiseBase *failure_parent = nullptr;
  Owner *child = nullptr;
  std::coroutine_handle<> (*completion)(void *) noexcept = nullptr;
  void *completion_state = nullptr;
  Error error;
  State state = State::created;
  CancelToken cancellation;
  bool cancellation_bound = false;
};

inline std::coroutine_handle<> propagate(PromiseBase &origin, Error error) noexcept
{
  auto *current = &origin;
  for (;;) {
    current->error = error;
    current->state = State::failed;

    if (!current->failure_parent) {
      if (current->completion)
        return current->completion(current->completion_state);

      return current->continuation;
    }

    current = current->failure_parent;
  }
}

// The owner at a recovery boundary reclaims the failed chain inside-out, without
// recursive destructor calls. Detach each child before destroying its parent.
struct Owner {
  PromiseBase *promise = nullptr;

  explicit Owner(PromiseBase *p = nullptr) noexcept : promise(p)
  {
  }

  Owner(Owner &&other) noexcept : promise(std::exchange(other.promise, nullptr))
  {
  }

  Owner(const Owner &) = delete;

  ~Owner()
  {
    reset();
  }

  void reset() noexcept
  {
    auto *current = std::exchange(promise, nullptr);
    if (current)
      current->failure_parent = nullptr;

    while (current) {
      require(current->state != State::running);
      auto *child = std::exchange(current->child, nullptr);

      if (child && child->promise) {
        auto *next = std::exchange(child->promise, nullptr);
        next->failure_parent = current;
        current = next;
      } else {
        auto *parent = current->failure_parent;
        current->self.destroy();
        current = parent;
      }
    }
  }
};

struct FailureAwaiter {
  // Tagged readiness avoids MSVC/ASan symmetric-transfer error C4737.
  Result<void> result;

  explicit FailureAwaiter(Error error) noexcept : result(std::unexpected(error))
  {
  }

  bool await_ready() const noexcept
  {
    return result.has_value();
  }

  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) noexcept
  {
    return propagate(parent.promise(), result.error());
  }

  void await_resume() const noexcept
  {
    require(result.has_value());
  }
};

struct CancellationAwaiter {
  CancelToken token;

  bool await_ready() const noexcept
  {
    return !token.stop_requested();
  }

  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) const noexcept
  {
    return propagate(parent.promise(), std::make_error_code(std::errc::operation_canceled));
  }

  void await_resume() const noexcept
  {
  }
};

template <class T, bool Capture>
class TaskAwaiter;

template <class T>
struct Return : PromiseBase {
  std::optional<T> value;

  template <class U>
    requires std::constructible_from<T, U &&>
  void return_value(U &&v) noexcept
  {
    static_assert(std::is_nothrow_constructible_v<T, U &&>);
    value.emplace(std::forward<U>(v));
  }

  void return_value(std::unexpected<Error> e) noexcept
  {
    error = e.error();
    state = State::failed;
  }
};

template <>
struct Return<void> : PromiseBase {
  void return_void() noexcept
  {
  }
};

template <class T>
struct Promise : Return<T> {
  static void *operator new(std::size_t size)
  {
    return weave::detail::allocate_frame(size, alignof(Promise));
  }

  static void operator delete(void *memory, std::size_t size) noexcept
  {
    weave::detail::deallocate_frame(memory, size, alignof(Promise));
  }

  Task<T> get_return_object() noexcept;

  std::suspend_always initial_suspend() const noexcept
  {
    return {};
  }

  struct Final {
    bool await_ready() const noexcept
    {
      return false;
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> handle) noexcept
    {
      auto &promise = handle.promise();
      if (promise.state == State::failed)
        return propagate(promise, promise.error);

      promise.state = State::succeeded;
      if (promise.completion)
        return promise.completion(promise.completion_state);

      return promise.continuation;
    }

    void await_resume() const noexcept
    {
    }
  };

  Final final_suspend() const noexcept
  {
    return {};
  }

  [[noreturn]] void unhandled_exception() const noexcept
  {
    std::abort();
  }

  template <class U>
  auto await_transform(Task<U> &&task) noexcept;

  auto await_transform(FailureAwaiter failure) noexcept
  {
    return failure;
  }

  auto await_transform(CancellationPoint) noexcept
  {
    return CancellationAwaiter{this->cancellation};
  }

  struct TokenAwaiter {
    CancelToken token;

    bool await_ready() const noexcept
    {
      return true;
    }

    void await_suspend(std::coroutine_handle<>) const noexcept
    {
    }

    CancelToken await_resume() const noexcept
    {
      return token;
    }
  };

  auto await_transform(GetCancellation) noexcept
  {
    return TokenAwaiter{this->cancellation};
  }

  template <class U>
  auto await_transform(Result<U>) noexcept = delete;

  template <class A>
  A &&await_transform(A &&awaiter) noexcept
  {
    return std::forward<A>(awaiter);
  }
};

} // namespace detail

template <class T>
class [[nodiscard]] Task {
  static_assert(std::is_void_v<T> || (!std::is_reference_v<T> && std::is_nothrow_move_constructible_v<T>));
  detail::Owner owner_;
  friend struct detail::Promise<T>;
  friend struct detail::TaskAccess;
  template <class U, bool Capture>
  friend class detail::TaskAwaiter;

  explicit Task(detail::PromiseBase *promise) noexcept : owner_(promise)
  {
  }

  detail::Owner release() && noexcept
  {
    return std::move(owner_);
  }

public:
  using value_type = T;
  using promise_type = detail::Promise<T>;
  Task(Task &&) noexcept = default;
  Task(const Task &) = delete;
  Task &operator=(const Task &) = delete;

  // Observe failure without changing the result; consume and own the task and observer.
  template <detail::ErrorObserver F>
  Task on_error(F observer) && noexcept;
};

namespace detail {

template <class T, bool Capture>
class TaskAwaiter {
  Owner owner_;
  PromiseBase *parent_ = nullptr;

  void detach() noexcept
  {
    if (parent_) {
      if (parent_->child == &owner_)
        parent_->child = nullptr;

      parent_ = nullptr;
    }
  }

public:
  explicit TaskAwaiter(Task<T> &&task) noexcept : owner_(std::move(task).release())
  {
  }

  TaskAwaiter(TaskAwaiter &&other) noexcept : owner_(std::move(other.owner_))
  {
    require(!other.parent_);
  }

  ~TaskAwaiter()
  {
    detach();
  }

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) noexcept
  {
    require(owner_.promise != nullptr);
    auto &promise = *owner_.promise;
    require(promise.state == State::created);

    promise.state = State::running;
    promise.continuation = parent;

    if constexpr (std::derived_from<P, PromiseBase>) {
      parent_ = &parent.promise();
      if (!promise.cancellation_bound)
        promise.cancellation = parent_->cancellation;

      // Operand evaluation may defer one await_resume until another operand
      // has been awaited. Only an unfinished child would make that unsafe.
      auto *child = parent_->child;
      auto child_running = child && child->promise && child->promise->state == State::running;
      require(!child_running);

      parent_->child = &owner_;
      if constexpr (!Capture)
        promise.failure_parent = parent_;
    } else {
      static_assert(Capture, "Use as_result() at a non-Task boundary");
    }

    if (promise.cancellation.stop_requested())
      return propagate(promise, std::make_error_code(std::errc::operation_canceled));
    return promise.self;
  }

  auto await_resume() noexcept -> std::conditional_t<Capture, Result<T>, T>
  {
    detach();
    auto &promise = static_cast<Promise<T> &>(*owner_.promise);

    if constexpr (Capture) {
      auto result = [&]() -> Result<T> {
        if (promise.state == State::failed)
          return std::unexpected(promise.error);

        require(promise.state == State::succeeded);
        if constexpr (std::is_void_v<T>)
          return {};
        else
          return std::move(*promise.value);
      }();

      owner_.reset();
      return result;
    } else {
      require(promise.state == State::succeeded);
      if constexpr (std::is_void_v<T>) {
        owner_.reset();
      } else {
        T result = std::move(*promise.value);
        owner_.reset();
        return result;
      }
    }
  }
};

template <class T>
Task<T> Promise<T>::get_return_object() noexcept
{
  this->self = std::coroutine_handle<Promise>::from_promise(*this);
  return Task<T>{this};
}

template <class T>
template <class U>
auto Promise<T>::await_transform(Task<U> &&task) noexcept
{
  return TaskAwaiter<U, false>{std::move(task)};
}

// Roots can fail at an ordinary await, so handle.done() is not a completion test.
struct TaskAccess {
  template <class T>
  static void bind(Task<T> &task, CancelToken token) noexcept
  {
    auto *promise = task.owner_.promise;
    require(promise && promise->state == State::created);
    promise->cancellation = std::move(token);
    promise->cancellation_bound = true;
  }

  template <class T>
  static void inherit(Task<T> &task, CancelToken token) noexcept
  {
    if (!task.owner_.promise->cancellation_bound)
      task.owner_.promise->cancellation = std::move(token);
  }

  template <class T>
  static void start(
    Task<T> &task,
    void *state = nullptr,
    std::coroutine_handle<> (*completed)(void *) noexcept = nullptr) noexcept
  {
    auto *promise = task.owner_.promise;
    require(promise && promise->state == State::created);

    promise->state = State::running;
    promise->completion = completed;
    promise->completion_state = state;
    if (promise->cancellation.stop_requested())
      propagate(*promise, std::make_error_code(std::errc::operation_canceled)).resume();
    else
      promise->self.resume();
  }

  template <class T>
  static bool done(const Task<T> &task) noexcept
  {
    require(task.owner_.promise != nullptr);
    const auto state = task.owner_.promise->state;
    return state == State::succeeded || state == State::failed;
  }

  template <class T>
  static bool failed(const Task<T> &task) noexcept
  {
    require(done(task));
    return task.owner_.promise->state == State::failed;
  }

  template <class T>
  static Result<T> take(Task<T> &task) noexcept
  {
    require(done(task));
    auto &promise = static_cast<Promise<T> &>(*task.owner_.promise);

    auto result = [&]() -> Result<T> {
      if (promise.state == State::failed)
        return std::unexpected(promise.error);

      if constexpr (std::is_void_v<T>)
        return {};
      else
        return std::move(*promise.value);
    }();

    task.owner_.reset();
    return result;
  }
};

} // namespace detail

inline auto fail(Error error) noexcept
{
  return detail::FailureAwaiter{error};
}

inline auto fail(std::errc error) noexcept
{
  return fail(std::make_error_code(error));
}

template <class T>
auto as_result(Task<T> task) noexcept
{
  return detail::TaskAwaiter<T, true>{std::move(task)};
}

namespace detail {

template <class T, ErrorObserver F>
Task<T> observe_error(Task<T> task, F observer)
{
  auto result = co_await as_result(std::move(task));
  if (!result)
    std::invoke(observer, std::as_const(result).error());

  if (!result)
    co_await fail(result.error());

  if constexpr (!std::is_void_v<T>)
    co_return std::move(*result);
}

template <std::size_t N>
struct AllAwaiter {
  std::array<Task<void>, N> operations;
  std::size_t remaining = N;
  std::coroutine_handle<> continuation{};
  bool armed = false;

  bool await_ready() const noexcept
  {
    return N == 0;
  }

  static std::coroutine_handle<> complete(void *state) noexcept
  {
    auto &self = *static_cast<AllAwaiter *>(state);
    --self.remaining;
    return self.remaining == 0 && self.armed ? self.continuation : std::noop_coroutine();
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> caller) noexcept
  {
    continuation = caller;
    for (auto &operation : operations) {
      TaskAccess::inherit(operation, caller.promise().cancellation);
      TaskAccess::start(operation, this, complete);
    }

    armed = true;
    return remaining != 0;
  }

  Result<void> await_resume() noexcept
  {
    Result<void> result;
    for (auto &operation : operations) {
      auto child = TaskAccess::take(operation);
      if (result && !child)
        result = std::unexpected(child.error());
    }

    return result;
  }
};

} // namespace detail

template <class T>
template <detail::ErrorObserver F>
Task<T> Task<T>::on_error(F observer) && noexcept
{
  detail::require(owner_.promise && owner_.promise->state == detail::State::created);
  // This entry point is not a coroutine: the adapter must not borrow a temporary Task's this pointer.
  return detail::observe_error(std::move(*this), std::move(observer));
}

// Serialized join-all: drain every child before propagating the first error in
// argument order. No implicit sibling cancellation or concurrent graph resumption.
template <class... Operations>
  requires(std::same_as<Operations, Task<void>> && ...)
Task<void> when_all(Operations... operations)
{
  auto result = co_await detail::AllAwaiter<sizeof...(Operations)>{{std::move(operations)...}};
  if (!result)
    co_await fail(result.error());
}

} // namespace weave
