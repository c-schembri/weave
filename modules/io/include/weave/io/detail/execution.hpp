#pragma once

#include <weave/cancellation.hpp>

namespace weave {

class Context;

namespace detail {

struct Executor;

struct Posted {
  void (*invoke)(void *) noexcept = nullptr;
  void *state = nullptr;
  Context *target = nullptr;
  Executor *executor = nullptr;
  Posted *next = nullptr;
};

void post(Context &context, Posted &message) noexcept;
CancelToken context_cancellation(Context &context) noexcept;

struct TaskObserver {
  void *state = nullptr;
  void (*spawned)(void *) noexcept = nullptr;
  void (*finished)(void *) noexcept = nullptr;
};

// IO routes a continuation without knowing the scheduler or owning the root task.
struct Executor {
  void (*schedule)(Posted &) noexcept = nullptr;
  // Optional native-completion fast path; submission and cleanup always use schedule.
  void (*dispatch_completion)(Posted &) noexcept = nullptr;
};

inline thread_local Context *current_context = nullptr;
inline thread_local Executor *current_executor = nullptr;
inline thread_local unsigned inline_budget = 0;

} // namespace detail

} // namespace weave
