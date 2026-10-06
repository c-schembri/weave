#include <weave/io.hpp>
#include "common.hpp"

namespace bench::workloads {

static __declspec(noinline) weave::Task<weave::u64> weave_value(weave::u64 input)
{
  co_return input + 1;
}

static __declspec(noinline) asio::awaitable<weave::u64> asio_value(weave::u64 input)
{
  co_return input + 1;
}

static weave::Task<void> weave_calls(benchmark::State &state)
{
  weave::u64 value = 0;
  for (auto _ : state) {
    value = co_await weave_value(value);
    benchmark::DoNotOptimize(value);
  }
  if (value != static_cast<weave::u64>(state.iterations()))
    state.SkipWithError("Incorrect coroutine result");
}

static asio::awaitable<void> asio_calls(benchmark::State &state)
{
  weave::u64 value = 0;
  for (auto _ : state) {
    value = co_await asio_value(value);
    benchmark::DoNotOptimize(value);
  }
  if (value != static_cast<weave::u64>(state.iterations()))
    state.SkipWithError("Incorrect coroutine result");
}

static void WeaveCoroutine(benchmark::State &state)
{
  auto ctx = weave::Context::create();
  if (!ctx) {
    state.SkipWithError("Context failed");
    return;
  }

  bench::Profile profile(*ctx);
  if (!ctx->run(weave_calls(state)))
    state.SkipWithError("Call task failed");
  profile.report(state);
  state.SetItemsProcessed(state.iterations());
}

static void AsioCoroutine(benchmark::State &state)
{
  asio::io_context ctx;
  asio::co_spawn(ctx, asio_calls(state), bench::completed);
  ctx.run();
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(WeaveCoroutine)->UseRealTime()->Unit(benchmark::kNanosecond);
BENCHMARK(AsioCoroutine)->UseRealTime()->Unit(benchmark::kNanosecond);

} // namespace bench::workloads
