#include "common.hpp"
#include "cpu_work.hpp"
#include "callback_pool.hpp"

using support::LibuvClient;
using support::UsocketsClient;

template <class Client, bool Skew>
static void native_cpu(benchmark::State &state)
{
  const auto workers = static_cast<std::size_t>(state.range(0));
  constexpr std::size_t count = 32;
  support::CallbackPool<Client> pool(workers, 0, 0);

  struct Job {
    support::CallbackLoop::Command command;
    std::optional<std::promise<weave::u64>> result;
    std::size_t seed = 0, workers = 0;
  };

  std::array<Job, count> jobs;
  std::vector<std::future<weave::u64>> results;
  results.reserve(count);
  weave::u64 expected = 0;
  for (std::size_t i = 0; i < count; ++i) {
    jobs[i].seed = i + 1;
    jobs[i].workers = workers;
    jobs[i].command = {&jobs[i], [](void *data) {
                         auto &job = *static_cast<Job *>(data);
                         const auto value = Skew ? bench::skew_work(job.seed, job.workers)
                                                 : bench::integer_work(job.seed);
                         auto result = std::move(*job.result);
                         job.result.reset();
                         result.set_value(value);
                       }};
    expected ^= Skew ? bench::skew_work(i + 1, workers) : bench::integer_work(i + 1);
  }
  for (auto _ : state) {
    for (std::size_t i = 0; i < count; ++i) {
      jobs[i].result.emplace();
      results.push_back(jobs[i].result->get_future());
      pool.loop(i).post(jobs[i].command);
    }
    weave::u64 result = 0;
    for (auto &job : results)
      result ^= job.get();
    results.clear();
    benchmark::DoNotOptimize(result);
    if (result != expected) {
      state.SkipWithError("Native CPU result mismatch");
      break;
    }
  }
  pool.stop();
  state.SetItemsProcessed(state.iterations() * count);
}

#define REGISTER_NATIVE_CPU(Client, Prefix)     \
  BENCHMARK_TEMPLATE(native_cpu, Client, false) \
    ->Name(Prefix "MulticoreCpu")               \
    ->Arg(1)                                    \
    ->Arg(2)                                    \
    ->Arg(4)                                    \
    ->Arg(8)                                    \
    ->UseRealTime()                             \
    ->Unit(benchmark::kMicrosecond);            \
  BENCHMARK_TEMPLATE(native_cpu, Client, true)  \
    ->Name(Prefix "AffineSkew")                 \
    ->Arg(1)                                    \
    ->Arg(2)                                    \
    ->Arg(4)                                    \
    ->Arg(8)                                    \
    ->UseRealTime()                             \
    ->Unit(benchmark::kMicrosecond)

REGISTER_NATIVE_CPU(LibuvClient, "Libuv");
REGISTER_NATIVE_CPU(UsocketsClient, "Usockets");
#undef REGISTER_NATIVE_CPU
