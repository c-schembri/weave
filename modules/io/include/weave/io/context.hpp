#pragma once

#include <weave/core.hpp>
#include <weave/io/detail/execution.hpp>
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
public:
  explicit Context(ContextOptions options = {}) noexcept;
  ~Context();
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  Result<void> status() const noexcept;
  bool stop_requested() const noexcept;

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
    enter(false, nullptr);
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
  struct Impl;
  std::unique_ptr<Impl> impl_;
  void check_thread() const noexcept;
  void enter(bool managed, void *scheduler_group) noexcept;
  void leave() noexcept;
  void post(detail::Posted &message) noexcept;
  void cancel_pending() noexcept;
  void poll(bool wait = true);
  void count(u64 &counter, u64 amount = 1) noexcept;
};

} // namespace weave
