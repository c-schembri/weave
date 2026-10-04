#pragma once

#include <weave/io.hpp>
#include <windows.h>
#include <array>
#include <atomic>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace weave {

struct Context::Impl {
  HANDLE port_ = nullptr;
  Error error_;
  std::thread::id owner_ = std::this_thread::get_id();
  std::array<OVERLAPPED_ENTRY, 64> completions_{};
  Metrics metrics_{};
  std::size_t handles_ = 0;
  bool running_ = false;
  ContextOptions options_;
  unsigned inline_budget_ = 0;
  bool managed_ = false;
  std::atomic<bool> stopping_{false};
  std::vector<std::uintptr_t> managed_handles_;
  void *scheduler_group_ = nullptr;
  std::shared_mutex io_mutex_;
};

namespace detail {

inline constexpr ULONG_PTR posted_key = 1;
inline constexpr ULONG_PTR wake_key = 2;

// The OS-owned record stays in the suspended caller's frame until it is dequeued.
struct Operation {
  OVERLAPPED overlapped{};
  Posted event{};
  DWORD transferred = 0;
  Error error{};
  bool failed = false;
};

// Private backend contract, shared only with native IO providers such as TCP.
struct IoAccess {
  static Context::Impl &state(Context &context) noexcept
  {
    return *context.impl_;
  }

  static void check_thread(const Context &context) noexcept
  {
    context.check_thread();
  }

  static void count(Context &context, u64 &counter, u64 amount = 1) noexcept
  {
    context.count(counter, amount);
  }

  static Result<bool> attach(Context &context, std::uintptr_t handle, bool skip_success);
  static Result<void> close(Context &context, std::uintptr_t handle, Error (*close_native)(std::uintptr_t) noexcept);
};

} // namespace detail

} // namespace weave
