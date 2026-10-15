#pragma once
#include <weave/io.hpp>
#include "asio_config.hpp"
#include <benchmark/benchmark.h>

namespace bench {

struct Profile {
  weave::Context &ctx;
  weave::Context::Metrics io_before;
  weave::detail::FrameMetrics frames_before;

  explicit Profile(weave::Context &context)
      : ctx(context), io_before(context.metrics()), frames_before(weave::detail::frame_metrics)
  {
  }

  void report(benchmark::State &state) const
  {
#if defined(WEAVE_PROFILE_RUNTIME)
    if (state.iterations() == 0)
      return;
    const weave::f64 iterations = static_cast<weave::f64>(state.iterations());
    const auto &io = ctx.metrics();
    state.counters["frames/iteration"] = (weave::detail::frame_metrics.allocations - frames_before.allocations) /
      iterations;
    state.counters["frame_bytes/iteration"] = (weave::detail::frame_metrics.bytes - frames_before.bytes) / iterations;
    state.counters["frame_heap_allocs/iteration"] = (weave::detail::frame_metrics.heap_allocations -
                                                      frames_before.heap_allocations) /
      iterations;
    state.counters["frame_heap_bytes/iteration"] = (weave::detail::frame_metrics.heap_bytes -
                                                     frames_before.heap_bytes) /
      iterations;
    state.counters["frame_cache_hits/iteration"] = (weave::detail::frame_metrics.cache_hits -
                                                     frames_before.cache_hits) /
      iterations;
    state.counters["reads/iteration"] = (io.read_calls - io_before.read_calls) / iterations;
    state.counters["writes/iteration"] = (io.write_calls - io_before.write_calls) / iterations;
    state.counters["inline_success/iteration"] = (io.immediate_successes - io_before.immediate_successes) / iterations;
    state.counters["packets/iteration"] = (io.completed - io_before.completed) / iterations;
    state.counters["dequeues/iteration"] = (io.dequeue_calls - io_before.dequeue_calls) / iterations;
    state.counters["fairness_posts/iteration"] = (io.fairness_posts - io_before.fairness_posts) / iterations;
    state.counters["inline_completions/iteration"] = (io.inline_completions - io_before.inline_completions) /
      iterations;
    state.counters["submission_ns/iteration"] = (io.submission_ns - io_before.submission_ns) / iterations;
    state.counters["read_submission_ns/iteration"] = (io.read_submission_ns - io_before.read_submission_ns) /
      iterations;
    state.counters["write_submission_ns/iteration"] = (io.write_submission_ns - io_before.write_submission_ns) /
      iterations;
    state.counters["dequeue_ns/iteration"] = (io.dequeue_ns - io_before.dequeue_ns) / iterations;
#else
    (void)state;
#endif
  }
};

inline constexpr auto use_result = asio::as_tuple(asio::use_awaitable);

inline void completed(std::exception_ptr failure)
{
  if (failure)
    std::abort();
}

} // namespace bench
