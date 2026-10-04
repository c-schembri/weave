#include <weave/runtime.hpp>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
  const bool steal = argc == 2 && std::strcmp(argv[1], "steal") == 0;
  if (argc > 2 || (argc == 2 && !steal))
    return 1;
  weave::Runtime runtime(
    {.workers = 4, .scheduler = steal ? weave::Scheduler::work_stealing : weave::Scheduler::worker_affine});
  if (!runtime.status())
    return 1;
  auto root = runtime.spawn([&runtime](weave::Context &) -> weave::Task<int> {
    auto first = co_await runtime.spawn([](weave::Context &ctx) -> weave::Task<int> {
      co_await ctx.yield();
      co_return 20;
    });
    auto second = co_await runtime.spawn([](weave::Context &) -> weave::Task<int> { co_return 22; });
    const auto a = co_await std::move(first);
    const auto b = co_await std::move(second);
    co_return a + b;
  });
  if (!root)
    return 1;
  const auto result = std::move(*root).get();
  runtime.join();
  if (!result) {
    std::fprintf(stderr, "%s\n", result.error().message().c_str());
    return 1;
  }
  std::printf(
    "%zu workers, %s, result %d\n",
    runtime.worker_count(),
    steal ? "work-stealing" : "worker-affine",
    *result);
  return result == 42 ? 0 : 1;
}
