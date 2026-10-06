#include <weave/runtime.hpp>
#include <cstdio>
#include <cstring>

int main(int argc, char **argv)
{
  const bool steal = argc == 2 && std::strcmp(argv[1], "steal") == 0;
  if (argc > 2 || (argc == 2 && !steal))
    return 1;
  auto runtime = weave::Runtime::create(
    {.workers = 4, .scheduler = steal ? weave::Scheduler::work_stealing : weave::Scheduler::worker_affine});
  if (!runtime)
    return 1;
  const auto result = runtime->run([&runtime]() -> weave::Task<int> {
    auto first = runtime->spawn([](weave::Context &ctx) -> weave::Task<int> {
      co_await ctx.yield();
      co_return 20;
    });
    if (!first)
      co_await weave::fail(first.error());
    auto second = runtime->spawn([]() -> weave::Task<int> { co_return 22; });
    if (!second)
      co_await weave::fail(second.error());
    const auto a = co_await std::move(*first);
    const auto b = co_await std::move(*second);
    co_return a + b;
  });
  if (!result) {
    std::fprintf(stderr, "%s\n", result.error().message().c_str());
    return 1;
  }
  std::printf(
    "%zu workers, %s, result %d\n",
    runtime->worker_count(),
    steal ? "work-stealing" : "worker-affine",
    *result);
  return result == 42 ? 0 : 1;
}
