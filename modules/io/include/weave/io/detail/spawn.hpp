#pragma once

#include <weave/io/join_handle.hpp>
#include <functional>

namespace weave::detail {

struct ChildObserver {
  void *state = nullptr;
  void (*finished)(void *, Result<void>) noexcept = nullptr;
};

struct SpawnBase {
  Posted event;
  Context *context = nullptr;
  void (*retain)(SpawnBase *) noexcept = nullptr;
  void (*release_scheduled)(SpawnBase *) noexcept = nullptr;
  void (*on_finish)(SpawnBase *) noexcept = nullptr;
  ChildObserver child_observer;
};

template <class F>
  requires std::invocable<F &, Context &>
auto invoke_factory(F &factory, Context &context) -> std::invoke_result_t<F &, Context &>
{
  return std::invoke(factory, context);
}

template <class F>
  requires(!std::invocable<F &, Context &> && std::invocable<F &>)
auto invoke_factory(F &factory, Context &) -> std::invoke_result_t<F &>
{
  return std::invoke(factory);
}

template <class T>
struct SpawnValue {};

template <class T>
struct SpawnValue<Task<T>> {
  using type = T;
};

template <class F>
using SpawnResult = typename SpawnValue<decltype(invoke_factory(std::declval<F &>(), std::declval<Context &>()))>::type;

template <class F>
concept SpawnFactory = std::constructible_from<std::decay_t<F>, F> &&
  requires { typename SpawnResult<std::decay_t<F>>; };

// Transfer an already-created frame without adding a wrapper coroutine.
template <class T>
struct TaskSubmission {
  Task<T> operation;

  Task<T> operator()() noexcept
  {
    return std::move(operation);
  }
};

enum class SpawnMode {
  joinable,
  detached
};

struct IgnoreError {
  void operator()(const Error &) const noexcept
  {
  }
};

template <class F, class Base, class H = IgnoreError>
struct SpawnTask : Base, JoinState<SpawnResult<F>> {
  using T = SpawnResult<F>;
  std::optional<F> factory;
  std::optional<Task<T>> root;
  std::optional<H> error_handler;

  explicit SpawnTask(F &&f, SpawnMode mode = SpawnMode::joinable, H handler = {}, SpawnOptions options = {})
      : factory(std::move(f)), error_handler(std::move(handler))
  {
    this->group_link.emplace(options.cancel.native_token(), ForwardCancel{this->cancellation});
    // Detached results must be reclaimed by execution, never by a submitting thread's temporary handle.
    if (mode == SpawnMode::detached)
      this->references.store(1, std::memory_order_relaxed);
    this->destroy = [](JoinStateBase *state) noexcept { delete static_cast<SpawnTask *>(state); };
    this->event.state = this;
    this->event.invoke = start;
    this->retain = [](SpawnBase *state) noexcept {
      static_cast<SpawnTask *>(state)->references.fetch_add(1, std::memory_order_relaxed);
    };
    this->release_scheduled = [](SpawnBase *state) noexcept { static_cast<SpawnTask *>(state)->release(); };
  }

  JoinHandle<T> handle() noexcept
  {
    return JoinHandle<T>{this};
  }

  void report_error(const Error &error) noexcept
  {
    std::invoke(*error_handler, error);
  }

  static void start(void *state) noexcept
  {
    auto *self = static_cast<SpawnTask *>(state);
    require(current_context != nullptr);
    self->context = current_context;
    self->context_link.emplace(context_cancellation(*self->context).native_token(), ForwardCancel{self->cancellation});
    if (self->cancellation.stop_requested()) {
      self->result.emplace(std::unexpected(std::make_error_code(std::errc::operation_canceled)));
      completed(self);
      return;
    }
    self->root.emplace(invoke_factory(*self->factory, *self->context));
    TaskAccess::bind(*self->root, self->cancellation.token());
    TaskAccess::start(*self->root, self, completed);
  }

  static std::coroutine_handle<> completed(void *state) noexcept
  {
    auto *self = static_cast<SpawnTask *>(state);
    // Reclaim frames only after the completion callback and resumption have unwound.
    self->event.invoke = finish;
    post(*self->context, self->event);
    return std::noop_coroutine();
  }

  static void finish(void *state) noexcept
  {
    auto *self = static_cast<SpawnTask *>(state);
    if (self->root)
      self->result.emplace(TaskAccess::take(*self->root));
    else
      self->factory.reset();
    self->root.reset();
    self->context_link.reset();
    self->group_link.reset();
    if (!*self->result)
      self->report_error(self->result->error());
    self->error_handler.reset();
    self->factory.reset();
    Result<void> outcome;
    if (!*self->result)
      outcome = std::unexpected(self->result->error());
    const auto observer = self->child_observer;
    self->publish();
    if (observer.finished)
      observer.finished(observer.state, std::move(outcome));
    self->on_finish(self);
  }
};

} // namespace weave::detail
