#pragma once

#include <weave/io.hpp>
#include <liburing.h>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace weave {

namespace detail {

struct TimerRecord {
  std::chrono::steady_clock::time_point deadline;
  Context *context = nullptr;
  Posted event{};
  Error error;
  std::size_t index = std::numeric_limits<std::size_t>::max();
};

struct TimerQueue {
  std::mutex mutex;
  std::vector<TimerRecord *> heap;

  void insert(TimerRecord &timer);
  void remove(TimerRecord &timer) noexcept;
  void swap(std::size_t a, std::size_t b) noexcept;
  void repair(std::size_t index) noexcept;
  std::chrono::nanoseconds wait_time() noexcept;
  bool dispatch_due() noexcept;
};

struct IoDomain {};

// Submission and cancellation commands are driven by the ring's owning Context.
// The frame remains suspended until both the native request and cancel CQEs drain.
struct Operation {
  enum class Kind {
    other,
    receive,
    send
  } kind = Kind::other;
  Posted event{};
  Posted submission{};
  Posted cancellation{};
  Context *context = nullptr;
  void *provider = nullptr;
  void (*prepare)(io_uring_sqe *, Operation &) noexcept = nullptr;
  std::mutex mutex;
  int socket = -1;
  int result = 0;
  bool submitted = false;
  bool cancelled = false;
  bool cancel_pending = false;
  bool native_done = false;
};

} // namespace detail

struct Context::Impl {
  struct Completion {
    u64 key;
    int result;
  };

  io_uring ring_{};
  std::vector<Completion> buffered_completions_;
  std::size_t next_completion_ = 0;
  int wake_fd_ = -1;
  u64 wake_value_ = 0;
  std::mutex ready_mutex_;
  detail::Posted *ready_first_ = nullptr;
  detail::Posted *ready_last_ = nullptr;
  std::thread::id owner_ = std::this_thread::get_id();
  Metrics metrics_{};
  std::size_t handles_ = 0;
  bool running_ = false;
  ContextOptions options_;
  std::atomic<bool> stopping_{false};
  void *scheduler_group_ = nullptr;
  std::mutex io_mutex_;
  std::vector<detail::Operation *> operations_;
  std::vector<std::uintptr_t> registered_handles_;
  std::mutex submissions_;
  bool closing_ = false;
  std::atomic<std::size_t> active_{0};
  detail::TaskObserver observer_;
  CancelSource stop_source_;
  detail::TimerQueue timers_;
};

namespace detail {

struct IoAccess {
  static Result<Context> create_context(ContextOptions options, unsigned entries = 256) noexcept;

  static Context::Impl &state(Context &context) noexcept
  {
    return *context.impl_;
  }

  static void check_thread(const Context &context) noexcept
  {
    context.check_thread();
  }

  static void check_execution(const Context &context) noexcept
  {
    context.check_thread();
    require(current_context != nullptr);
    const auto group = context.impl_->scheduler_group_;
    const bool same_context = current_context == &context;
    const bool same_group = group && current_executor && current_context->impl_->scheduler_group_ == group;
    if (!same_context && !same_group) {
      std::fprintf(stderr, "Weave I/O: incompatible execution Context\n");
      require(false);
    }
  }

  static void count(Context &context, u64 &counter, u64 amount = 1) noexcept
  {
    context.count(counter, amount);
  }

  static Result<bool> attach(Context &context, std::uintptr_t handle, bool skip_success);
  static Result<void> close(Context &context, std::uintptr_t handle, Error (*close_native)(std::uintptr_t) noexcept);
  static void submit(Operation &operation) noexcept;
  static void cancel(Operation &operation) noexcept;
  static void cancel_socket(Context &context, int socket) noexcept;
  static void start_timer(TimerRecord &timer);
  static void cancel_timer(TimerRecord &timer) noexcept;

  static TimerQueue &timers(Context &context) noexcept
  {
    return context.impl_->timers_;
  }
};

} // namespace detail

} // namespace weave
