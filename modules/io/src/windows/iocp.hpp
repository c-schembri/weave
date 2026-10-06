#pragma once

#include <weave/io.hpp>
#include <windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>
#include <chrono>
#include <limits>

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
  DWORD wait_time() noexcept;
  bool dispatch_due() noexcept;
};

struct IoDomain {
  HANDLE port = nullptr;
  TaskObserver collectors;
  TimerQueue timers;
  std::mutex waiters_mutex;
  std::vector<HANDLE> waiters;
  void wake_waiters() noexcept;
  ~IoDomain();
};

} // namespace detail

struct Context::Impl {
  HANDLE port_ = nullptr;
  std::shared_ptr<detail::IoDomain> domain_;
  HANDLE thread_ = nullptr;
  std::mutex ready_mutex_;
  detail::Posted *ready_first_ = nullptr;
  detail::Posted *ready_last_ = nullptr;
  std::thread::id owner_ = std::this_thread::get_id();
  std::array<OVERLAPPED_ENTRY, 64> completions_{};
  Metrics metrics_{};
  std::size_t handles_ = 0;
  bool running_ = false;
  ContextOptions options_;
  unsigned inline_budget_ = 0;
  std::atomic<bool> stopping_{false};
  bool cancelled_ = false;
  std::vector<std::uintptr_t> registered_handles_;
  void *scheduler_group_ = nullptr;
  std::shared_mutex io_mutex_;
  std::mutex submissions_;
  bool closing_ = false;
  std::atomic<std::size_t> active_{0};
  detail::TaskObserver observer_;
  CancelSource stop_source_;
  detail::TimerQueue timers_;
};

namespace detail {

// The OS-owned record stays in the suspended caller's frame until it is dequeued.
struct Operation {
  OVERLAPPED overlapped{};
  Posted event{};
  Context *context = nullptr;
  DWORD transferred = 0;
  Error error{};
  bool failed = false;
};

// Private backend contract, shared only with native IO providers such as TCP.
struct IoAccess {
  // The injected OS entry point lets tests cover setup failure without exhausting handles.
  static Result<Context> create_context(
    ContextOptions options,
    decltype(&CreateIoCompletionPort) create_port,
    const std::shared_ptr<IoDomain> &domain = {}) noexcept;

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
  static void start_timer(TimerRecord &timer);
  static void cancel_timer(TimerRecord &timer) noexcept;

  static TimerQueue &timers(Context &context) noexcept
  {
    auto &state = *context.impl_;
    return state.domain_ ? state.domain_->timers : state.timers_;
  }

  static Result<void> close(Context &context, std::uintptr_t handle, Error (*close_native)(std::uintptr_t) noexcept);
};

} // namespace detail

} // namespace weave
