#include <benchmark/benchmark.h>
#include <weave/core/detail/frame_allocator.hpp>
#if defined(WEAVE_BENCH_NATIVE)
#include <uv.h>
#endif

// SkipWithError otherwise does not make the benchmark executable fail in CI.
class CheckingReporter final : public benchmark::ConsoleReporter {
public:
  bool failed = false;

  void ReportRuns(const std::vector<Run> &runs) override
  {
    for (const auto &run : runs) {
      if (run.skipped == benchmark::internal::SkippedWithError)
        failed = true;
    }
    ConsoleReporter::ReportRuns(runs);
  }
};

int main(int argc, char **argv)
{
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv))
    return 1;
  benchmark::AddCustomContext("weave_recycle_frames", WEAVE_RECYCLE_FRAMES ? "on" : "off");
#if defined(WEAVE_BENCH_NATIVE)
  benchmark::AddCustomContext("libuv_version", uv_version_string());
  benchmark::AddCustomContext("usockets_revision", "7a7c820db4740c2a2a4faf0d918c4eec2ac1fac3");
  benchmark::AddCustomContext("usockets_backend", "libuv readiness polling; TCP only, SSL disabled");
  benchmark::AddCustomContext(
    "native_clients",
    "callback APIs; one loop per worker; benchmark submission queue and std::future joins");
#endif
#if defined(WEAVE_PROFILE_RUNTIME)
  benchmark::AddCustomContext("weave_profile_runtime", "on");
#else
  benchmark::AddCustomContext("weave_profile_runtime", "off");
#endif
#if defined(ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING)
  benchmark::AddCustomContext("asio_recycle_frames", "off (diagnostic only)");
#else
  benchmark::AddCustomContext("asio_recycle_frames", "on");
#endif
  CheckingReporter reporter;
  const auto matched = benchmark::RunSpecifiedBenchmarks(&reporter);
  benchmark::Shutdown();
  return reporter.failed || matched == 0 ? 1 : 0;
}
