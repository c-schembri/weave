#include <weave/io.hpp>
#include "common.hpp"

using Value = weave::u64;
using Outcome = weave::Result<Value>;

static __declspec(noinline) weave::Task<Value> explicit_chain(int depth, bool fail)
{
  if (depth == 1) {
    if (fail)
      co_return std::unexpected(std::make_error_code(std::errc::io_error));
    co_return Value{1};
  }
  auto child = co_await weave::as_result(explicit_chain(depth - 1, fail));
  if (!child)
    co_return std::unexpected(child.error());
  co_return *child + 1;
}

static __declspec(noinline) weave::Task<Value> weave_chain(int depth, bool fail)
{
  if (depth == 1) {
    if (fail)
      co_await weave::fail(std::errc::io_error);
    co_return Value{1};
  }
  co_return (co_await weave_chain(depth - 1, fail)) + 1;
}

static __declspec(noinline) weave::Task<Value> weave_return_chain(int depth, bool fail)
{
  if (depth == 1) {
    if (fail)
      co_return std::unexpected(std::make_error_code(std::errc::io_error));
    co_return Value{1};
  }
  co_return (co_await weave_return_chain(depth - 1, fail)) + 1;
}

static __declspec(noinline) asio::awaitable<Outcome> asio_chain(int depth, bool fail)
{
  if (depth == 1) {
    if (fail)
      co_return std::unexpected(std::make_error_code(std::errc::io_error));
    co_return Value{1};
  }
  auto child = co_await asio_chain(depth - 1, fail);
  if (!child)
    co_return std::unexpected(child.error());
  co_return *child + 1;
}

static bool check(benchmark::State &state, const Outcome &result, bool failure)
{
  bool valid;
  if (failure)
    valid = !result && result.error() == std::errc::io_error;
  else
    valid = result && *result == static_cast<Value>(state.range(0));

  if (!valid)
    state.SkipWithError("Incorrect fallible task outcome");
  benchmark::DoNotOptimize(result);
  return valid;
}

template <auto Chain = weave_chain>
static weave::Task<void> weave_calls(benchmark::State &state)
{
  weave::u64 index = 0;
  for (auto _ : state) {
    const bool failure = index++ % 100 < static_cast<Value>(state.range(1));
    auto result = co_await weave::as_result(Chain(static_cast<int>(state.range(0)), failure));
    if (!check(state, result, failure))
      co_return;
  }
}

static asio::awaitable<void> asio_calls(benchmark::State &state)
{
  weave::u64 index = 0;
  for (auto _ : state) {
    const bool failure = index++ % 100 < static_cast<Value>(state.range(1));
    auto result = co_await asio_chain(static_cast<int>(state.range(0)), failure);
    if (!check(state, result, failure))
      co_return;
  }
}

static void WeaveExplicitTask(benchmark::State &state)
{
  weave::Context context;
  if (!context.status()) {
    state.SkipWithError("Context failed");
    return;
  }
  bench::Profile profile(context);
  if (!context.run(weave_calls<explicit_chain>(state)))
    state.SkipWithError("Unexpected root failure");
  profile.report(state);
  state.SetItemsProcessed(state.iterations());
}

static void WeaveTask(benchmark::State &state)
{
  weave::Context context;
  if (!context.status()) {
    state.SkipWithError("Context failed");
    return;
  }
  bench::Profile profile(context);
  if (!context.run(weave_calls(state)))
    state.SkipWithError("Unexpected root failure");
  profile.report(state);
  state.SetItemsProcessed(state.iterations());
}

static void WeaveReturnTask(benchmark::State &state)
{
  weave::Context context;
  if (!context.status()) {
    state.SkipWithError("Context failed");
    return;
  }
  bench::Profile profile(context);
  if (!context.run(weave_calls<weave_return_chain>(state)))
    state.SkipWithError("Unexpected root failure");
  profile.report(state);
  state.SetItemsProcessed(state.iterations());
}

static void AsioTask(benchmark::State &state)
{
  asio::io_context context;
  asio::co_spawn(context, asio_calls(state), bench::completed);
  context.run();
  state.SetItemsProcessed(state.iterations());
}

static void arguments(benchmark::internal::Benchmark *b)
{
  for (int depth : {1, 8, 64}) {
    for (int failure : {0, 1, 100})
      b->Args({depth, failure});
  }
  b->ArgNames({"depth", "error_pct"})->UseRealTime()->Unit(benchmark::kNanosecond);
}

BENCHMARK(WeaveExplicitTask)->Apply(arguments);
BENCHMARK(WeaveTask)->Apply(arguments);
BENCHMARK(WeaveReturnTask)->Apply(arguments);
BENCHMARK(AsioTask)->Apply(arguments);
