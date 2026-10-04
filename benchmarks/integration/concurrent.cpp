#include <weave/tcp.hpp>
#include <weave/runtime.hpp>
#include "common.hpp"
#include "asio_pool.hpp"
#include "peer_process.hpp"
#include "benchmark_affinity.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <future>
#include <fstream>
#include <numeric>

namespace bench::concurrent {

using Clock = std::chrono::steady_clock;
using Outcome = weave::Result<void>;
using asio::ip::tcp;
static std::chrono::milliseconds duration{1000};
static experiment::CpuPartition cpu_partition;

struct Workload {
  std::size_t connections, workers, bytes;
  int work;
};

static constexpr std::array<Workload, 6> workloads{
  {{64, 4, 1024, 0},
    {1024, 1, 1024, 0},
    {1024, 4, 1024, 0},
    {1024, 8, 1024, 0},
    {256, 4, 65536, 0},
    {1024, 4, 1024, 512}}};

struct CpuTime {
  weave::u64 kernel = 0, user = 0, cycles = 0;

  static CpuTime read(HANDLE process)
  {
    FILETIME created{}, exited{}, kernel{}, user{};
    weave::detail::require(GetProcessTimes(process, &created, &exited, &kernel, &user) != 0);
    ULONG64 cycles = 0;
    weave::detail::require(QueryProcessCycleTime(process, &cycles) != 0);
    auto ticks = [](FILETIME t) { return (weave::u64{t.dwHighDateTime} << 32) | t.dwLowDateTime; };
    return {ticks(kernel), ticks(user), cycles};
  }

  weave::f64 seconds_since(CpuTime before) const
  {
    return static_cast<weave::f64>((kernel - before.kernel) + (user - before.user)) * 1e-7;
  }
};

struct Control {
  std::atomic<std::size_t> ready = 0, done = 0;
  std::atomic<bool> go = false;
  Clock::time_point deadline;
  const std::size_t count;
  const int warmups, extra_work;
  experiment::Handle ready_event, done_event;

  explicit Control(std::size_t n, int warmup_count = 4, int extra = 0)
      : count(n), warmups(warmup_count), extra_work(extra)
  {
    ready_event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    done_event.value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    weave::detail::require(ready_event.value && done_event.value);
  }

  void arrive()
  {
    if (ready.fetch_add(1, std::memory_order_acq_rel) + 1 == count)
      weave::detail::require(SetEvent(ready_event.value) != 0);
  }

  void leave()
  {
    if (!go.load(std::memory_order_acquire))
      weave::detail::require(SetEvent(ready_event.value) != 0);
    if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == count)
      weave::detail::require(SetEvent(done_event.value) != 0);
  }
};

struct OnExit {
  Control &control;

  ~OnExit()
  {
    control.leave();
  }
};

static __declspec(noinline) weave::u64 cpu_work(weave::u64 value, int iterations)
{
  for (int i = 0; i < iterations; ++i) {
    value = value * 1664525 + 1013904223;
    value ^= value >> 17;
  }
  return value;
}

struct Payload {
  std::vector<std::byte> tx, rx;
  std::vector<weave::f64> latency;
  std::size_t samples = 0;
  weave::u64 seed = 0, expected = 0;
  int work = 0;

  void initialize(std::size_t id, std::size_t bytes, std::size_t capacity, int iterations)
  {
    tx.resize(bytes);
    rx.resize(bytes);
    latency.resize(capacity);
    for (std::size_t i = 0; i < bytes; ++i)
      tx[i] = static_cast<std::byte>((id * 31 + i * 17) & 255);
    tx[0] = static_cast<std::byte>(id & 255);
    tx[1] = static_cast<std::byte>((id >> 8) & 255);
    seed = id + 1;
    work = iterations;
    expected = cpu_work(seed, work);
  }

  bool validate(int extra = 0) const
  {
    const auto value = cpu_work(seed, work);
    benchmark::DoNotOptimize(value);
    if (extra) {
      const auto injected = cpu_work(seed, extra);
      benchmark::DoNotOptimize(injected);
    }
    return value == expected && tx == rx;
  }

  bool record(Clock::time_point start)
  {
    if (samples == latency.size())
      return false;
    latency[samples++] = std::chrono::duration<weave::f64, std::micro>(Clock::now() - start).count();
    return true;
  }
};

struct WeaveConnection : Payload {
  std::optional<weave::TcpStream> socket;
};

static weave::Task<void> weave_explicit_session(weave::Context &ctx, WeaveConnection &p, Control &control)
{
  OnExit done{control};
  for (int i = 0; i < control.warmups; ++i) {
    auto sent = co_await weave::as_result(p.socket->write_all(p.tx));
    if (!sent)
      co_await weave::fail(sent.error());
    auto received = co_await weave::as_result(p.socket->read_exactly(p.rx));
    if (!received)
      co_await weave::fail(received.error());
    if (!p.validate())
      co_await weave::fail(std::errc::bad_message);
  }
  control.arrive();
  while (!control.go.load(std::memory_order_acquire))
    co_await ctx.yield();
  while (Clock::now() < control.deadline) {
    const auto start = Clock::now();
    auto sent = co_await weave::as_result(p.socket->write_all(p.tx));
    if (!sent)
      co_await weave::fail(sent.error());
    auto received = co_await weave::as_result(p.socket->read_exactly(p.rx));
    if (!received)
      co_await weave::fail(received.error());
    if (!p.validate(control.extra_work) || !p.record(start))
      co_await weave::fail(std::errc::bad_message);
  }
  co_return;
}

static weave::Task<void> weave_session(weave::Context &ctx, WeaveConnection &p, Control &control)
{
  OnExit done{control};
  for (int i = 0; i < control.warmups; ++i) {
    co_await p.socket->write_all(p.tx);
    co_await p.socket->read_exactly(p.rx);
    if (!p.validate())
      co_await weave::fail(std::errc::bad_message);
  }
  control.arrive();
  while (!control.go.load(std::memory_order_acquire))
    co_await ctx.yield();
  while (Clock::now() < control.deadline) {
    const auto start = Clock::now();
    co_await p.socket->write_all(p.tx);
    co_await p.socket->read_exactly(p.rx);
    if (!p.validate(control.extra_work) || !p.record(start))
      co_await weave::fail(std::errc::bad_message);
  }
}

static asio::awaitable<Outcome> asio_session(tcp::socket &socket, Payload &p, Control &control)
{
  OnExit done{control};
  for (int i = 0; i < control.warmups; ++i) {
    auto [send_error, sent] = co_await asio::async_write(socket, asio::buffer(p.tx), bench::use_result);
    if (send_error)
      co_return std::unexpected(send_error);
    auto [read_error, received] = co_await asio::async_read(socket, asio::buffer(p.rx), bench::use_result);
    if (read_error)
      co_return std::unexpected(read_error);
    if (sent != p.tx.size() || received != p.rx.size() || !p.validate())
      co_return std::unexpected(std::make_error_code(std::errc::bad_message));
  }
  control.arrive();
  while (!control.go.load(std::memory_order_acquire))
    co_await asio::post(asio::use_awaitable);
  while (Clock::now() < control.deadline) {
    const auto start = Clock::now();
    auto [send_error, sent] = co_await asio::async_write(socket, asio::buffer(p.tx), bench::use_result);
    if (send_error)
      co_return std::unexpected(send_error);
    auto [read_error, received] = co_await asio::async_read(socket, asio::buffer(p.rx), bench::use_result);
    if (read_error)
      co_return std::unexpected(read_error);
    if (sent != p.tx.size() || received != p.rx.size() || !p.validate() || !p.record(start))
      co_return std::unexpected(std::make_error_code(std::errc::bad_message));
  }
  co_return Outcome{};
}

struct Measurement {
  CpuTime client_before, peer_before;
  Clock::time_point start;
  weave::f64 wall = 0, client_cpu = 0, peer_cpu = 0;
  weave::u64 client_cycles = 0, peer_cycles = 0;

  bool begin(Control &control, std::size_t count, experiment::PeerProcess &peer)
  {
    const bool ready = WaitForSingleObject(control.ready_event.value, 20000) == WAIT_OBJECT_0 &&
      control.ready == count && control.done == 0;
    peer_before = CpuTime::read(peer.process());
    client_before = CpuTime::read(GetCurrentProcess());
    start = Clock::now();
    control.deadline = start + (ready ? duration : std::chrono::milliseconds(0));
    control.go.store(true, std::memory_order_release);
    return ready;
  }

  void end(experiment::PeerProcess &peer)
  {
    wall = std::chrono::duration<weave::f64>(Clock::now() - start).count();
    const auto client = CpuTime::read(GetCurrentProcess());
    const auto server = CpuTime::read(peer.process());
    client_cpu = client.seconds_since(client_before);
    peer_cpu = server.seconds_since(peer_before);
    client_cycles = client.cycles - client_before.cycles;
    peer_cycles = server.cycles - peer_before.cycles;
  }

  template <class P>
  bool collect(benchmark::UserCounters &counters, const std::vector<P> &payloads, std::vector<weave::f64> &latency)
  {
    latency.clear();
    std::size_t count = 0, least = static_cast<std::size_t>(-1), most = 0;
    for (const auto &p : payloads) {
      count += p.samples;
      least = std::min(least, p.samples);
      most = std::max(most, p.samples);
    }
    if (!count || !least)
      return false;
    latency.reserve(count);
    for (const auto &p : payloads)
      latency.insert(latency.end(), p.latency.begin(), p.latency.begin() + p.samples);
    std::sort(latency.begin(), latency.end());
    auto percentile = [&](weave::f64 q) { return latency[static_cast<std::size_t>(std::ceil(q * count)) - 1]; };
    counters["p50_us"] = percentile(0.50);
    counters["p95_us"] = percentile(0.95);
    counters["p99_us"] = percentile(0.99);
    counters["p999_us"] = percentile(0.999);
    counters["max_us"] = latency.back();
    counters["client_cpu_us_per_op"] = client_cpu * 1e6 / count;
    counters["client_cycles_per_op"] = static_cast<weave::f64>(client_cycles) / count;
    counters["peer_cycles_per_op"] = static_cast<weave::f64>(peer_cycles) / count;
    counters["client_cores"] = client_cpu / wall;
    counters["peer_cores"] = peer_cpu / wall;
    counters["roundtrips_per_second"] = count / wall;
    counters["samples"] = static_cast<weave::f64>(count);
    counters["min_connection_samples"] = static_cast<weave::f64>(least);
    counters["max_connection_samples"] = static_cast<weave::f64>(most);
    counters["wall_seconds"] = wall;
    return true;
  }
};

static std::size_t sample_capacity(std::size_t connections)
{
  return std::max<std::size_t>(1024, 2'000'000 * static_cast<std::size_t>(duration.count()) / 1000 / connections);
}

static bool weave_window(
  bool task,
  weave::Runtime &runtime,
  std::vector<WeaveConnection> &connections,
  experiment::PeerProcess &peer,
  Workload config,
  weave::Scheduler scheduler,
  Measurement &measurement,
  int warmups = 4,
  int extra_work = 0)
{
  const auto count = config.connections;
  Control control(count, warmups, extra_work);
  std::vector<weave::JoinHandle<void>> jobs;
  jobs.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    connections[i].samples = 0;
    auto factory = [&, i](weave::Context &ctx) {
      if (task)
        return weave_session(ctx, connections[i], control);
      return weave_explicit_session(ctx, connections[i], control);
    };
    auto job = scheduler == weave::Scheduler::work_stealing ? runtime.spawn(factory)
                                                            : runtime.spawn_on(i % config.workers, factory);
    weave::detail::require(static_cast<bool>(job));
    jobs.push_back(std::move(*job));
  }
  bool ok = measurement.begin(control, count, peer);
  if (!ok || WaitForSingleObject(control.done_event.value, 20000) != WAIT_OBJECT_0) {
    peer.stop();
    ok = false;
  }
  for (auto &job : jobs)
    ok = static_cast<bool>(std::move(job).get()) && ok;
  measurement.end(peer);
  return ok;
}

template <class F>
static bool with_weave(Workload config, weave::Scheduler scheduler, F body)
{
  const auto count = config.connections, workers = config.workers;
  experiment::PeerProcess peer(cpu_partition.peer);
  if (!peer.valid())
    return false;
  std::vector<WeaveConnection> connections(count);
  weave::Runtime runtime({.workers = workers, .scheduler = scheduler});
  if (!runtime.status())
    return false;
  bool ok = true;
  for (std::size_t i = 0; i < count; ++i) {
    connections[i].initialize(i, config.bytes, sample_capacity(count), config.work);
    auto setup = runtime.spawn_on(i % workers, [&, i](weave::Context &ctx) -> weave::Task<void> {
      auto socket = co_await weave::tcp::connect(ctx, "127.0.0.1", peer.port(i));
      co_await socket.no_delay();
      connections[i].socket.emplace(std::move(socket));
    });
    weave::detail::require(static_cast<bool>(setup));
    if (!std::move(*setup).get()) {
      ok = false;
      break;
    }
  }
  if (ok)
    ok = body(runtime, connections, peer);
  // Have the peer actively close first, avoiding client ephemeral-port exhaustion.
  ok = peer.stop() && ok;
  for (std::size_t i = 0; i < count; ++i) {
    if (!connections[i].socket)
      continue;
    auto cleanup = runtime.spawn_on(i % workers, [&, i](weave::Context &) -> weave::Task<void> {
      connections[i].socket.reset();
      co_return;
    });
    weave::detail::require(static_cast<bool>(cleanup));
    weave::detail::require(static_cast<bool>(std::move(*cleanup).get()));
  }
  runtime.join();
  return ok;
}

static bool asio_window(
  bench::AsioPool &pool,
  std::vector<tcp::socket> &sockets,
  std::vector<Payload> &payloads,
  experiment::PeerProcess &peer,
  Workload config,
  Measurement &measurement,
  int warmups = 4)
{
  Control control(config.connections, warmups);
  std::vector<std::future<Outcome>> jobs;
  jobs.reserve(config.connections);
  for (std::size_t i = 0; i < config.connections; ++i) {
    payloads[i].samples = 0;
    jobs.push_back(
      asio::co_spawn(
        pool.context(i % config.workers),
        asio_session(sockets[i], payloads[i], control),
        asio::use_future));
  }
  bool ok = measurement.begin(control, config.connections, peer);
  if (!ok || WaitForSingleObject(control.done_event.value, 20000) != WAIT_OBJECT_0) {
    peer.stop();
    ok = false;
  }
  for (auto &job : jobs)
    ok = static_cast<bool>(job.get()) && ok;
  measurement.end(peer);
  return ok;
}

template <class F>
static bool with_asio(Workload config, bool sharded, F body)
{
  const auto count = config.connections, workers = config.workers;
  experiment::PeerProcess peer(cpu_partition.peer);
  if (!peer.valid())
    return false;
  bench::AsioPool pool(workers, sharded);
  std::vector<tcp::socket> sockets;
  std::vector<Payload> payloads(count);
  sockets.reserve(count);
  asio::error_code error;
  for (std::size_t i = 0; i < count; ++i) {
    payloads[i].initialize(i, config.bytes, sample_capacity(count), config.work);
    sockets.emplace_back(pool.context(i % workers));
    sockets.back().connect(tcp::endpoint(asio::ip::address_v4::loopback(), peer.port(i)), error);
    if (error) {
      peer.stop();
      return false;
    }
    sockets.back().set_option(tcp::no_delay(true), error);
    if (error) {
      peer.stop();
      return false;
    }
  }
  bool ok = body(pool, sockets, payloads, peer);
  ok = peer.stop() && ok;
  for (auto &socket : sockets) {
    socket.close(error);
    ok = !error && ok;
  }
  return ok;
}

static Workload arguments_from(const benchmark::State &state)
{
  return {
    static_cast<std::size_t>(state.range(0)),
    static_cast<std::size_t>(state.range(1)),
    static_cast<std::size_t>(state.range(2)),
    static_cast<int>(state.range(3))};
}

template <bool Task>
static void weave_concurrent(benchmark::State &state, weave::Scheduler scheduler)
{
  const auto config = arguments_from(state);
  const bool ok = with_weave(config, scheduler, [&](auto &runtime, auto &connections, auto &peer) {
    Measurement measurement;
    std::vector<weave::f64> scratch;
    for (auto _ : state) {
      const bool success = weave_window(Task, runtime, connections, peer, config, scheduler, measurement);
      state.SetIterationTime(measurement.wall);
      if (!success || !measurement.collect(state.counters, connections, scratch))
        return false;
      const auto samples = static_cast<weave::i64>(state.counters["samples"]);
      state.SetItemsProcessed(samples);
      state.SetBytesProcessed(samples * static_cast<weave::i64>(config.bytes) * 2);
    }
    return true;
  });
  if (!ok)
    state.SkipWithError("Weave fixture, exchange, progress, or drain failed");
}

template <bool Sharded>
static void asio_concurrent(benchmark::State &state)
{
  const auto config = arguments_from(state);
  const bool ok = with_asio(config, Sharded, [&](auto &pool, auto &sockets, auto &payloads, auto &peer) {
    Measurement measurement;
    std::vector<weave::f64> scratch;
    for (auto _ : state) {
      const bool success = asio_window(pool, sockets, payloads, peer, config, measurement);
      state.SetIterationTime(measurement.wall);
      if (!success || !measurement.collect(state.counters, payloads, scratch))
        return false;
      const auto samples = static_cast<weave::i64>(state.counters["samples"]);
      state.SetItemsProcessed(samples);
      state.SetBytesProcessed(samples * static_cast<weave::i64>(config.bytes) * 2);
    }
    return true;
  });
  if (!ok)
    state.SkipWithError("Asio fixture, exchange, progress, or drain failed");
}

static void WeaveExplicitConcurrentAffine(benchmark::State &state)
{
  weave_concurrent<false>(state, weave::Scheduler::worker_affine);
}

static void WeaveConcurrentAffine(benchmark::State &state)
{
  weave_concurrent<true>(state, weave::Scheduler::worker_affine);
}

static void WeaveExplicitConcurrentStealing(benchmark::State &state)
{
  weave_concurrent<false>(state, weave::Scheduler::work_stealing);
}

static void WeaveConcurrentStealing(benchmark::State &state)
{
  weave_concurrent<true>(state, weave::Scheduler::work_stealing);
}

static void AsioConcurrentAffine(benchmark::State &state)
{
  asio_concurrent<true>(state);
}

static void AsioConcurrentShared(benchmark::State &state)
{
  asio_concurrent<false>(state);
}

static void arguments(benchmark::internal::Benchmark *b)
{
  for (const auto &w : workloads) {
    b->Args(
      {static_cast<weave::i64>(w.connections),
        static_cast<weave::i64>(w.workers),
        static_cast<weave::i64>(w.bytes),
        w.work});
  }

  b->ArgNames({"connections", "workers", "bytes", "cpu"})
    ->Iterations(1)
    ->UseManualTime()
    ->Unit(benchmark::kMillisecond);
}

BENCHMARK(WeaveExplicitConcurrentAffine)->Apply(arguments);
BENCHMARK(WeaveConcurrentAffine)->Apply(arguments);
BENCHMARK(WeaveExplicitConcurrentStealing)->Apply(arguments);
BENCHMARK(WeaveConcurrentStealing)->Apply(arguments);
BENCHMARK(AsioConcurrentAffine)->Apply(arguments);
BENCHMARK(AsioConcurrentShared)->Apply(arguments);

static int paired_run(const std::string &path, bool smoke, int injected_work)
{
  std::ofstream output(path);
  if (!output)
    return 1;
  benchmark::JSONReporter reporter;
  reporter.SetOutputStream(&output);
  reporter.ReportContext(benchmark::BenchmarkReporter::Context{});
  const int blocks = smoke ? 2 : 7;
  int sequence = 0;
  bool ok = true;
  for (int scheduler_id = 0; scheduler_id != 2 && ok; ++scheduler_id) {
    const auto scheduler = scheduler_id == 0 ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing;
    for (std::size_t index = 0; index != (smoke ? 1 : workloads.size()) && ok; ++index) {
      const auto config = workloads[index];
      const auto fixture = static_cast<int>(scheduler_id * workloads.size() + index);
      std::vector<weave::f64> scratch;
      auto emit = [&](
                    Measurement &measurement,
                    const auto &payloads,
                    int phase,
                    int block,
                    int slot,
                    int label,
                    int implementation) {
        benchmark::BenchmarkReporter::Run run;
        run.family_index = scheduler_id;
        run.per_family_instance_index = static_cast<weave::i64>(index);
        run.repetitions = blocks;
        run.repetition_index = block;
        run.statistics = nullptr;
        run.memory_result = {};
        run.time_unit = benchmark::kMillisecond;
        run.real_accumulated_time = measurement.wall;
        run.cpu_accumulated_time = measurement.client_cpu;
        run.run_name.function_name = scheduler_id == 0 ? "PairedAffine" : "PairedStealing";
        run.run_name.args = "connections:" + std::to_string(config.connections) +
          "/workers:" + std::to_string(config.workers) + "/bytes:" + std::to_string(config.bytes) +
          "/cpu:" + std::to_string(config.work);
        const bool valid = measurement.collect(run.counters, payloads, scratch);
        if (!valid) {
          run.skipped = benchmark::internal::SkippedWithError;
          run.skip_message = "No samples or a connection failed to progress";
        }
        run.counters["fixture_id"] = fixture;
        run.counters["phase"] = phase;
        run.counters["block"] = block;
        run.counters["slot"] = slot;
        run.counters["label"] = label;
        run.counters["implementation"] = implementation;
        run.counters["sequence"] = sequence++;
        reporter.ReportRuns({run});
        output.flush();
        return valid && output.good();
      };
      ok = with_weave(config, scheduler, [&](auto &runtime, auto &connections, auto &peer) {
        Measurement measurement;
        benchmark::UserCounters ignored;
        // Warm both implementations, the sockets, and the latency collector before pairing.
        for (const bool task : {false, true, true, false}) {
          if (!weave_window(task, runtime, connections, peer, config, scheduler, measurement) ||
            !measurement.collect(ignored, connections, scratch))
            return false;
        }
        for (int block = 0; block != blocks; ++block) {
          for (int phase_order = 0; phase_order != 2; ++phase_order) {
            const int phase = (block + fixture + phase_order) % 2;
            // ABBA/BAAB cancels a linear trend in log metrics within each block.
            for (int slot = 0; slot != 4; ++slot) {
              const int label = ((slot == 1 || slot == 2) ? 1 : 0) ^ ((block + fixture + phase) % 2);
              const bool task = phase == 1 && label == 1;
              if (!weave_window(
                    task,
                    runtime,
                    connections,
                    peer,
                    config,
                    scheduler,
                    measurement,
                    0,
                    task ? injected_work : 0) ||
                !emit(measurement, connections, phase, block, slot, label, task ? 1 : 0))
                return false;
            }
          }
        }
        return true;
      });
      if (ok) {
        ok = with_asio(config, scheduler_id == 0, [&](auto &pool, auto &sockets, auto &payloads, auto &peer) {
          Measurement measurement;
          benchmark::UserCounters ignored;
          for (int warmup = 0; warmup != 2; ++warmup) {
            if (!asio_window(pool, sockets, payloads, peer, config, measurement) ||
              !measurement.collect(ignored, payloads, scratch))
              return false;
          }
          for (int block = 0; block != blocks; ++block) {
            if (!asio_window(pool, sockets, payloads, peer, config, measurement, 0) ||
              !emit(measurement, payloads, 2, block, 0, 2, 2))
              return false;
          }
          return true;
        });
      }
      std::printf("Paired fixture %d: %s (%d windows)\n", fixture, ok ? "complete" : "FAILED", sequence);
      std::fflush(stdout);
    }
  }
  reporter.Finalize();
  output.flush();
  return ok && output.good() ? 0 : 1;
}

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

} // namespace bench::concurrent

int main(int argc, char **argv)
{
  using bench::concurrent::CheckingReporter;
  using bench::concurrent::cpu_partition;
  using bench::concurrent::duration;
  using bench::concurrent::paired_run;

  if (argc == 2 && std::string_view(argv[1]) == "--weave-peer")
    return experiment::serve_peer();
  bool isolate_cpus = false;
  bool paired_smoke = false;
  std::string paired_output;
  int injected_work = 0;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]), prefix = "--weave_duration_ms=";
    if (arg == "--weave_isolate_cpus") {
      isolate_cpus = true;
    } else if (arg == "--weave_paired_smoke") {
      paired_smoke = true;
    } else if (arg.starts_with("--weave_paired_out=")) {
      paired_output = arg.substr(std::string_view("--weave_paired_out=").size());
    } else if (arg == "--weave_paired_positive_control") {
      injected_work = 200000;
    } else if (arg.starts_with(prefix)) {
      int value = 0;
      const auto text = arg.substr(prefix.size());
      const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
      if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < 50 || value > 10000)
        return 1;
      duration = std::chrono::milliseconds(value);
    } else
      continue;
    for (int j = i; j < argc; ++j)
      argv[j] = argv[j + 1];
    --argc;
    --i;
  }
  if ((paired_smoke || injected_work) && paired_output.empty())
    return 1;
  if (injected_work && !paired_smoke)
    return 1;
  if (!paired_output.empty())
    duration = std::chrono::milliseconds(paired_smoke ? 50 : 250);
  if (isolate_cpus) {
    cpu_partition = experiment::isolated_cpus();
    if (!cpu_partition.client || !cpu_partition.peer ||
      !SetProcessAffinityMask(GetCurrentProcess(), cpu_partition.client)) {
      std::fputs("CPU isolation requires 12 available physical cores in one processor group.\n", stderr);
      return 1;
    }
  }
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv))
    return 1;
  benchmark::AddCustomContext(
    "measurement",
    "closed-loop RTT including client validation/CPU work; one long-lived root per connection");
  benchmark::AddCustomContext(
    "cpu",
    "GetProcessTimes and QueryProcessCycleTime; client and separate four-worker peer counted independently");
  benchmark::AddCustomContext("duration_ms", std::to_string(duration.count()));
  benchmark::AddCustomContext("client_affinity_mask", std::to_string(cpu_partition.client));
  benchmark::AddCustomContext("peer_affinity_mask", std::to_string(cpu_partition.peer));
  benchmark::AddCustomContext(
    "cpu_placement",
    isolate_cpus ? "8 client / 4 peer physical cores; one logical per core" : "OS default");
  benchmark::AddCustomContext("asio_revision", "366dfc44640182cb21c1ebf7efb658a6bec13f5a");
  benchmark::AddCustomContext(
    "reference",
    "Current Task with explicit as_result checks; not a historical release baseline");
  if (!paired_output.empty()) {
    benchmark::AddCustomContext("protocol", "weave-task-policy-5m-v1");
    benchmark::AddCustomContext("blocks", paired_smoke ? "2" : "7");
    benchmark::AddCustomContext("smoke", paired_smoke ? "true" : "false");
    benchmark::AddCustomContext("injected_work", std::to_string(injected_work));
    benchmark::AddCustomContext("weave_warmup_windows", "4");
    benchmark::AddCustomContext("asio_warmup_windows", "2");
    const int result = paired_run(paired_output, paired_smoke, injected_work);
    benchmark::Shutdown();
    return result;
  }
  CheckingReporter reporter;
  const auto cases = benchmark::RunSpecifiedBenchmarks(&reporter);
  benchmark::Shutdown();
  return reporter.failed || !cases ? 1 : 0;
}
