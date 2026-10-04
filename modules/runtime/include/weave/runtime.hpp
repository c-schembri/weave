#pragma once

#include <weave/io.hpp>
#include <weave/io/detail/context_access.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>

namespace weave {

class Runtime;
template <class T>
class JoinHandle;

namespace detail {

struct SpawnBase;
inline thread_local SpawnBase *current_task = nullptr;
void schedule(Posted &message) noexcept;

inline char join_completed;

struct JoinStateBase {
  std::atomic<unsigned> references{2}; // One owner in the runtime, one in the handle.
  std::atomic<void *> completion{nullptr};
  void (*destroy)(JoinStateBase *) noexcept = nullptr;

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
      ContextAccess::post(*message.target, message);
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

struct SpawnBase : Executor {
  Posted event;
  Context *context = nullptr;
  Runtime *runtime = nullptr;
  std::mutex ready_mutex;
  Posted *first = nullptr;
  Posted *last = nullptr;
  bool scheduled = false;
  bool pinned = false;
  bool scheduler_done = false;
  std::size_t worker = 0;
  void (*retain)(SpawnBase *) noexcept = nullptr;
  void (*release_scheduled)(SpawnBase *) noexcept = nullptr;
};
template <class F>
using SpawnResult = typename std::invoke_result_t<F &, Context &>::value_type;
template <class F>
struct SpawnTask;

} // namespace detail

// Dropping a handle releases the result, not the running task. Runtime owns it until completion.
template <class T>
class [[nodiscard]] JoinHandle {
  detail::JoinState<T> *state_;
  friend class Runtime;

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

  bool ready() const noexcept
  {
    detail::require(state_ != nullptr);
    return state_->ready();
  }

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
      message.invoke = [](void *address) noexcept { std::coroutine_handle<>::from_address(address).resume(); };

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
    if constexpr (std::is_void_v<T>)
      co_await std::move(result);
    else
      co_return co_await std::move(result);
  }

public:
  Task<T> as_task() && noexcept
  {
    detail::require(state_ != nullptr);
    return wait(std::move(*this));
  }

  auto operator co_await() && noexcept
  {
    return detail::TaskAwaiter<T, false>{std::move(*this).as_task()};
  }
};

template <class T>
auto as_result(JoinHandle<T> handle) noexcept
{
  return as_result(std::move(handle).as_task());
}

enum class Scheduler {
  worker_affine,
  work_stealing
};

struct RuntimeOptions {
  std::size_t workers = 0; // Zero uses hardware_concurrency(), or one if unknown.
  Scheduler scheduler = Scheduler::worker_affine;
  ContextOptions context{};
};

class Runtime {
public:
  explicit Runtime(RuntimeOptions options = {});
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;
  Result<void> status() const noexcept;
  std::size_t worker_count() const noexcept;
  Scheduler scheduler() const noexcept;
  bool stop_requested() const noexcept;
  void request_stop() noexcept;
  void join();     // Close submissions and drain accepted work without cancelling it.
  void shutdown(); // Request I/O cancellation, then join all workers.

  template <class F>
  auto spawn(F &&factory) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>;
  template <class F>
  auto spawn_on(std::size_t worker, F &&factory) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>;

private:
  friend void detail::schedule(detail::Posted &) noexcept;
  template <class F>
  friend struct detail::SpawnTask;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Result<void> submit(detail::SpawnBase &task, std::size_t worker);
  void finished() noexcept;
};

namespace detail {

template <class F>
struct SpawnTask : SpawnBase, JoinState<SpawnResult<F>> {
  using T = SpawnResult<F>;
  std::optional<F> factory;
  std::optional<Task<T>> root;

  explicit SpawnTask(F &&f) : factory(std::move(f))
  {
    this->destroy = [](JoinStateBase *state) noexcept { delete static_cast<SpawnTask *>(state); };
    this->event.state = this;
    this->event.invoke = start;
    this->retain = [](SpawnBase *state) noexcept {
      static_cast<SpawnTask *>(state)->references.fetch_add(1, std::memory_order_relaxed);
    };
    this->release_scheduled = [](SpawnBase *state) noexcept { static_cast<SpawnTask *>(state)->release(); };
  }

  static void start(void *state) noexcept
  {
    auto *self = static_cast<SpawnTask *>(state);
    if (current_task)
      self->context = current_context;

    self->root.emplace(std::invoke(*self->factory, *self->context));
    TaskAccess::start(*self->root, self, completed);
  }

  static std::coroutine_handle<> completed(void *state) noexcept
  {
    auto *self = static_cast<SpawnTask *>(state);
    // Reclaim a completed or failed root on its owner after resumption unwinds.
    self->event.invoke = finish;
    ContextAccess::post(*self->context, self->event);
    return std::noop_coroutine();
  }

  static void finish(void *state) noexcept
  {
    auto *self = static_cast<SpawnTask *>(state);
    self->result.emplace(TaskAccess::take(*self->root));
    self->root.reset();
    self->factory.reset();
    self->publish();

    if (self->event.executor) {
      // The scheduler releases its execution reference before decrementing active roots.
      self->scheduler_done = true;
      self->release();
      return;
    }

    self->runtime->finished();
    self->release();
  }
};

} // namespace detail

template <class F>
auto Runtime::spawn(F &&factory) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>
{
  return spawn_on(static_cast<std::size_t>(-1), std::forward<F>(factory));
}

template <class F>
auto Runtime::spawn_on(std::size_t worker, F &&factory) -> Result<JoinHandle<detail::SpawnResult<std::decay_t<F>>>>
{
  using Function = std::decay_t<F>;
  using T = detail::SpawnResult<Function>;

  auto *task = new (std::nothrow) detail::SpawnTask<Function>(Function(std::forward<F>(factory)));
  detail::require(task != nullptr);

  auto accepted = submit(*task, worker);
  if (!accepted) {
    delete task;
    return std::unexpected(accepted.error());
  }

  return JoinHandle<T>{task};
}

} // namespace weave
