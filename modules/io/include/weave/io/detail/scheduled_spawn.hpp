#pragma once

#include <weave/io/detail/spawn.hpp>
#include <cstddef>
#include <mutex>

namespace weave::detail {

// Storage for independently scheduled roots; scheduling policy stays in the coordinator.
struct ScheduledSpawn : SpawnBase, Executor {
  void *owner = nullptr;
  std::mutex ready_mutex;
  Posted *first = nullptr;
  Posted *last = nullptr;
  bool scheduled = false;
  bool pinned = false;
  bool scheduler_done = false;
  std::size_t worker = 0;
  ScheduledSpawn *queue_next = nullptr;
  ScheduledSpawn *queue_previous = nullptr;
};

struct SubmissionScope {
  void *state = nullptr;
  std::size_t worker = 0;
  Result<void> (*submit)(void *, std::size_t, ScheduledSpawn &) noexcept = nullptr;
};

inline thread_local const SubmissionScope *current_submission = nullptr;

} // namespace weave::detail
