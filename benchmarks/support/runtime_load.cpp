#include "asio_config.hpp"
#include "asio_pool.hpp"
#include "benchmark_affinity.hpp"
#include "runtime_workload.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using asio::ip::tcp;
using Clock = std::chrono::steady_clock;
static constexpr auto use_result = asio::as_tuple(asio::use_awaitable);

struct CpuTime {
  std::uint64_t ticks = 0, cycles = 0;

  static CpuTime read()
  {
    FILETIME created{}, exited{}, kernel{}, user{};
    ULONG64 cycles = 0;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) ||
      !QueryProcessCycleTime(GetCurrentProcess(), &cycles))
      std::abort();
    auto ticks = [](FILETIME time) { return (std::uint64_t{time.dwHighDateTime} << 32) | time.dwLowDateTime; };
    return {ticks(kernel) + ticks(user), cycles};
  }
};

struct Control {
  std::atomic<std::size_t> ready = 0, done = 0, errors = 0;
  std::atomic<bool> reported = false;
  HANDLE ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Clock::time_point deadline;
  std::size_t count;
  std::uint16_t port;

  void fail(std::size_t id, const char *stage, asio::error_code error, std::size_t transferred, std::size_t expected)
  {
    if (reported.exchange(true, std::memory_order_relaxed))
      return;
    std::fprintf(
      stderr,
      "Connection %zu failed: stage=%s code=%d category=%s message=%s transferred=%zu expected=%zu\n",
      id,
      stage,
      error.value(),
      error.category().name(),
      error.message().c_str(),
      transferred,
      expected);
    std::fflush(stderr);
  }

  ~Control()
  {
    CloseHandle(ready_event);
    CloseHandle(done_event);
  }
};

struct Connection {
  enum class Stage {
    connecting,
    configuring,
    warmup_write,
    warmup_read,
    waiting,
    measuring,
    finished,
    count
  };

  static constexpr std::array
    names{"connect", "no_delay", "warmup_write", "warmup_read", "gate", "measurement", "finished"};

  tcp::socket socket;
  asio::steady_timer gate;
  std::vector<std::byte> tx, expected, rx;
  std::vector<double> latency;
  std::size_t worker;
  std::size_t id;
  std::atomic<Stage> stage = Stage::connecting;
  std::atomic<unsigned> warmed = 0;
  bool ok = true;

  Connection(
    asio::io_context &context,
    std::size_t id,
    std::size_t owner,
    bench::stress::Workload config,
    std::size_t capacity)
      : socket(context), gate(context), tx(config.bytes), rx(config.bytes), worker(owner), id(id)
  {
    for (std::size_t i = 0; i < tx.size(); ++i)
      tx[i] = static_cast<std::byte>((id * 31 + i * 17) & 255);
    const auto seed = static_cast<std::uint64_t>(id + 1);
    std::memcpy(tx.data(), &seed, sizeof(seed));
    expected = tx;
    bench::stress::transform(expected, config);
    gate.expires_at(Clock::time_point::max());
    latency.reserve(capacity);
  }
};

static asio::awaitable<bool> exchange(Connection &connection, Control &control, bool record)
{
  const auto start = Clock::now();
  if (!record)
    connection.stage.store(Connection::Stage::warmup_write, std::memory_order_relaxed);
  auto [send_error, sent] = co_await asio::async_write(connection.socket, asio::buffer(connection.tx), use_result);
  if (send_error || sent != connection.tx.size()) {
    control.fail(
      connection.id,
      record ? "write" : "warmup_write",
      send_error ? send_error : std::make_error_code(std::errc::io_error),
      sent,
      connection.tx.size());
    co_return false;
  }
  if (!record)
    connection.stage.store(Connection::Stage::warmup_read, std::memory_order_relaxed);
  auto [read_error, received] = co_await asio::async_read(connection.socket, asio::buffer(connection.rx), use_result);
  if (read_error || received != connection.rx.size()) {
    control.fail(
      connection.id,
      record ? "read" : "warmup_read",
      read_error ? read_error : std::make_error_code(std::errc::io_error),
      received,
      connection.rx.size());
    co_return false;
  }
  if (connection.rx != connection.expected) {
    control
      .fail(connection.id, "validation", std::make_error_code(std::errc::bad_message), received, connection.rx.size());
    co_return false;
  }
  if (record)
    connection.latency.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
  co_return true;
}

static asio::awaitable<void> session(Connection &connection, Control &control)
{
  auto [error] = co_await connection.socket.async_connect(
    tcp::endpoint(asio::ip::address_v4::loopback(), control.port),
    use_result);
  if (error) {
    control.fail(connection.id, "connect", error, 0, 0);
    connection.ok = false;
    co_return;
  }
  connection.stage.store(Connection::Stage::configuring, std::memory_order_relaxed);
  connection.socket.set_option(tcp::no_delay(true), error);
  if (error) {
    control.fail(connection.id, "no_delay", error, 0, 0);
    connection.ok = false;
    co_return;
  }
  for (int i = 0; i < 8; ++i) {
    if (!co_await exchange(connection, control, false)) {
      connection.ok = false;
      co_return;
    }
    connection.warmed.store(i + 1, std::memory_order_relaxed);
  }
  connection.stage.store(Connection::Stage::waiting, std::memory_order_relaxed);
  if (control.ready.fetch_add(1, std::memory_order_acq_rel) + 1 == control.count)
    SetEvent(control.ready_event);
  auto [gate_error] = co_await connection.gate.async_wait(use_result);
  if (gate_error != asio::error::operation_aborted) {
    control.fail(connection.id, "gate", gate_error, 0, 0);
    connection.ok = false;
    co_return;
  }
  connection.stage.store(Connection::Stage::measuring, std::memory_order_relaxed);
  while (Clock::now() < control.deadline) {
    if (!co_await exchange(connection, control, true)) {
      connection.ok = false;
      co_return;
    }
  }
  connection.stage.store(Connection::Stage::finished, std::memory_order_relaxed);
}

static void report_progress(
  const char *phase,
  DWORD wait_result,
  const Control &control,
  const std::vector<std::unique_ptr<Connection>> &connections)
{
  std::fprintf(
    stderr,
    "%s failed: wait=%lu ready=%zu/%zu done=%zu errors=%zu\n",
    phase,
    wait_result,
    control.ready.load(),
    control.count,
    control.done.load(),
    control.errors.load());
  std::array<std::size_t, static_cast<std::size_t>(Connection::Stage::count)> stages{};
  unsigned reported = 0;
  for (const auto &connection : connections) {
    const auto stage = connection->stage.load(std::memory_order_relaxed);
    ++stages[static_cast<std::size_t>(stage)];
    if (stage != Connection::Stage::waiting && stage != Connection::Stage::finished && reported++ < 16) {
      std::fprintf(
        stderr,
        "Connection %zu: stage=%s warmup=%u/8\n",
        connection->id,
        Connection::names[static_cast<std::size_t>(stage)],
        connection->warmed.load(std::memory_order_relaxed));
    }
  }
  for (std::size_t i = 0; i < stages.size(); ++i)
    std::fprintf(stderr, "Stage %s: %zu connections\n", Connection::names[i], stages[i]);
  std::fflush(stderr);
}

int main(int argc, char **argv)
{
  if (argc == 2 && std::string_view{argv[1]} == "--cpu-masks") {
    const auto masks = experiment::isolated_cpus();
    if (!masks.client || !masks.peer)
      return 1;
    std::printf(
      "{\"client\":%llu,\"server\":%llu}\n",
      static_cast<unsigned long long>(masks.client),
      static_cast<unsigned long long>(masks.peer));
    return 0;
  }
  bench::stress::Workload config;
  std::uint16_t port = 0;
  std::size_t count = 0, workers = 0;
  unsigned uneven = 0;
  int duration_ms = 0;
  std::uintptr_t mask = 0;
  if (argc != 9 || !bench::stress::number(argv[1], port) || !bench::stress::number(argv[2], count) ||
    !bench::stress::number(argv[3], config.bytes) || !bench::stress::number(argv[4], config.work) ||
    !bench::stress::number(argv[5], uneven) || !bench::stress::number(argv[6], duration_ms) ||
    !bench::stress::number(argv[7], mask) || !bench::stress::number(argv[8], workers) || !port || count == 0 ||
    count > 8192 || workers == 0 || workers > 32 || uneven > 1 || duration_ms < 50 || duration_ms > 10000 ||
    !bench::stress::valid(config)) {
    std::fputs(
      "Usage: weave_runtime_load port connections bytes cpu uneven duration-ms affinity-mask workers\n",
      stderr);
    return 1;
  }
  config.uneven = uneven != 0;
  if (mask && !SetProcessAffinityMask(GetCurrentProcess(), mask))
    return 1;
  Control control{.count = count, .port = port};
  if (!control.ready_event || !control.done_event)
    return 1;
  bench::AsioPool pool(workers, true);
  std::vector<std::unique_ptr<Connection>> connections;
  connections.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto owner = i % workers;
    const auto capacity = std::max<std::size_t>(256, 2'000'000 * static_cast<std::size_t>(duration_ms) / 1000 / count);
    auto connection = std::make_unique<Connection>(pool.context(owner), i, owner, config, capacity);
    auto &state = *connection;
    connections.push_back(std::move(connection));
    asio::co_spawn(pool.context(owner), session(state, control), [&control, &state](std::exception_ptr error) {
      if (error || !state.ok) {
        control.errors.fetch_add(1, std::memory_order_relaxed);
        SetEvent(control.ready_event);
      }
      if (control.done.fetch_add(1, std::memory_order_acq_rel) + 1 == control.count)
        SetEvent(control.done_event);
    });
  }
  const auto ready_wait = WaitForSingleObject(control.ready_event, 20000);
  if (ready_wait != WAIT_OBJECT_0 || control.ready != count || control.errors) {
    report_progress("Load generator setup or warmup", ready_wait, control, connections);
    // Pending operations still borrow state; the supervisor owns process cleanup.
    ExitProcess(2);
  }
  std::puts("READY");
  std::fflush(stdout);
  std::string command;
  if (!std::getline(std::cin, command) || command != "GO")
    ExitProcess(2);
  const auto before = CpuTime::read();
  const auto start = Clock::now();
  control.deadline = start + std::chrono::milliseconds(duration_ms);
  for (std::size_t worker = 0; worker < workers; ++worker) {
    asio::post(pool.context(worker), [&, worker] {
      for (auto &connection : connections) {
        if (connection->worker == worker)
          connection->gate.cancel();
      }
    });
  }
  const auto done_wait = WaitForSingleObject(control.done_event, duration_ms + 15000);
  if (done_wait != WAIT_OBJECT_0 || control.errors) {
    report_progress("Load generator exchange, validation or progress", done_wait, control, connections);
    ExitProcess(2);
  }
  const auto wall = std::chrono::duration<double>(Clock::now() - start).count();
  const auto after = CpuTime::read();
  std::puts("MEASURED");
  std::fflush(stdout);
  std::vector<double> latency;
  std::size_t samples = 0, least = static_cast<std::size_t>(-1), most = 0;
  for (const auto &connection : connections) {
    const auto size = connection->latency.size();
    samples += size;
    least = std::min(least, size);
    most = std::max(most, size);
  }
  if (!samples || !least)
    ExitProcess(2);
  latency.reserve(samples);
  for (const auto &connection : connections)
    latency.insert(latency.end(), connection->latency.begin(), connection->latency.end());
  std::sort(latency.begin(), latency.end());
  auto percentile = [&](
                      double quantile) { return latency[static_cast<std::size_t>(std::ceil(quantile * samples)) - 1]; };
  const auto cpu = static_cast<double>(after.ticks - before.ticks) * 1e-7;
  std::printf(
    "{\"samples\":%zu,\"min_connection_samples\":%zu,\"max_connection_samples\":%zu,"
    "\"wall_seconds\":%.9f,\"roundtrips_per_second\":%.9f,\"client_cores\":%.9f,"
    "\"client_cycles_per_op\":%.9f,\"p50_us\":%.9f,\"p95_us\":%.9f,\"p99_us\":%.9f,"
    "\"p999_us\":%.9f,\"max_us\":%.9f}\n",
    samples,
    least,
    most,
    wall,
    samples / wall,
    cpu / wall,
    static_cast<double>(after.cycles - before.cycles) / samples,
    percentile(0.5),
    percentile(0.95),
    percentile(0.99),
    percentile(0.999),
    latency.back());
  std::fflush(stdout);
  // Keep sockets open so the server actively closes first, avoiding client TIME_WAIT buildup.
  if (!std::getline(std::cin, command) || command != "STOP")
    ExitProcess(2);
  for (auto &connection : connections) {
    asio::error_code ignored;
    connection->socket.close(ignored);
  }
}
