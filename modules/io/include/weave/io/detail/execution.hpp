#pragma once

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

// IO routes a continuation without knowing the scheduler or owning the root task.
struct Executor {
  void (*schedule)(Posted &) noexcept = nullptr;
};

inline thread_local Context *current_context = nullptr;
inline thread_local Executor *current_executor = nullptr;
inline thread_local unsigned inline_budget = 0;

} // namespace detail

} // namespace weave
