#pragma once

#include <weave/types.hpp>

namespace weave::detail {

enum class TraceEvent : u32 {
  enqueue = 1,
  steal,
  execute_begin,
  execute_end,
  park_begin,
  park_end,
  wake,
  io_submit_begin,
  io_submit_end,
  io_wait_begin,
  io_wait_end,
  io_complete,
  local_completion
};

#if defined(WEAVE_TRACE_RUNTIME)
void trace(TraceEvent event, const void *object, u64 value = 0) noexcept;
#else
inline void trace(TraceEvent, const void *, u64 = 0) noexcept
{
}
#endif

} // namespace weave::detail
