#include <weave/runtime.hpp>
#include "common.hpp"
#include "asio_pool.hpp"
#include "cpu_work.hpp"
#include <future>

namespace bench::multicore {

static constexpr std::size_t jobs_per_batch = 32;
using bench::AsioPool;
using bench::integer_work;
using bench::skew_work;

static weave::Task<weave::u64> weave_cpu(weave::u64 seed)
{
  co_return integer_work(seed);
}

static asio::awaitable<weave::u64> asio_cpu(weave::u64 seed)
{
  co_return integer_work(seed);
}

static weave::Task<weave::u64> weave_skew(weave::u64 seed, std::size_t workers)
{
  co_return skew_work(seed, workers);
}

static asio::awaitable<weave::u64> asio_skew(weave::u64 seed, std::size_t workers)
{
  co_return skew_work(seed, workers);
}

static void weave_cpu_benchmark(benchmark::State &state, weave::Scheduler scheduler, bool skew = false)
{
  const auto workers = static_cast<std::size_t>(state.range(0));
  weave::Runtime runtime({.workers = workers, .scheduler = scheduler});
  if (!runtime.status()) {
    state.SkipWithError("Runtime setup failed");
    return;
  }
  weave::u64 expected = 0;
  for (std::size_t i = 0; i < jobs_per_batch; ++i)
    expected ^= skew ? skew_work(i + 1, workers) : integer_work(i + 1);
  std::vector<weave::JoinHandle<weave::u64>> jobs;
  jobs.reserve(jobs_per_batch);
  for (auto _ : state) {
    for (std::size_t i = 0; i < jobs_per_batch; ++i) {
      auto job = runtime.spawn(
        [seed = i + 1, workers, skew](weave::Context &) { return skew ? weave_skew(seed, workers) : weave_cpu(seed); });
      weave::detail::require(static_cast<bool>(job));
      jobs.push_back(std::move(*job));
    }
    weave::u64 result = 0;
    for (auto &job : jobs) {
      auto value = std::move(job).get();
      weave::detail::require(static_cast<bool>(value));
      result ^= *value;
    }
    jobs.clear();
    benchmark::DoNotOptimize(result);
    if (result != expected) {
      state.SkipWithError("CPU result mismatch");
      break;
    }
  }
  runtime.join();
  state.SetItemsProcessed(state.iterations() * jobs_per_batch);
}

static void asio_cpu_benchmark(benchmark::State &state, bool skew = false, bool sharded = false)
{
  const auto workers = static_cast<std::size_t>(state.range(0));
  AsioPool pool(workers, sharded);
  weave::u64 expected = 0;
  for (std::size_t i = 0; i < jobs_per_batch; ++i)
    expected ^= skew ? skew_work(i + 1, workers) : integer_work(i + 1);
  std::vector<std::future<weave::u64>> jobs;
  jobs.reserve(jobs_per_batch);
  for (auto _ : state) {
    for (std::size_t i = 0; i < jobs_per_batch; ++i) {
      jobs.push_back(
        asio::co_spawn(pool.context(i), skew ? asio_skew(i + 1, workers) : asio_cpu(i + 1), asio::use_future));
    }
    weave::u64 result = 0;
    for (auto &job : jobs)
      result ^= job.get();
    jobs.clear();
    benchmark::DoNotOptimize(result);
    if (result != expected) {
      state.SkipWithError("CPU result mismatch");
      break;
    }
  }
  state.SetItemsProcessed(state.iterations() * jobs_per_batch);
}

static void WeaveMulticoreCpu(benchmark::State &state)
{
  weave_cpu_benchmark(state, weave::Scheduler::worker_affine);
}

static void WeaveStealingCpu(benchmark::State &state)
{
  weave_cpu_benchmark(state, weave::Scheduler::work_stealing);
}

static void WeaveAffineSkew(benchmark::State &state)
{
  weave_cpu_benchmark(state, weave::Scheduler::worker_affine, true);
}

static void WeaveStealingSkew(benchmark::State &state)
{
  weave_cpu_benchmark(state, weave::Scheduler::work_stealing, true);
}

static void AsioMulticoreCpu(benchmark::State &state)
{
  asio_cpu_benchmark(state);
}

static void AsioMulticoreSkew(benchmark::State &state)
{
  asio_cpu_benchmark(state, true);
}

static void AsioAffineCpu(benchmark::State &state)
{
  asio_cpu_benchmark(state, false, true);
}

static void AsioAffineSkew(benchmark::State &state)
{
  asio_cpu_benchmark(state, true, true);
}

BENCHMARK(WeaveMulticoreCpu)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(AsioMulticoreCpu)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(WeaveStealingCpu)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(WeaveAffineSkew)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(WeaveStealingSkew)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(AsioMulticoreSkew)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(AsioAffineCpu)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(AsioAffineSkew)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);

} // namespace bench::multicore
