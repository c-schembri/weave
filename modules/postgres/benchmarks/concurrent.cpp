#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include "benchmark_affinity.hpp"
#else
#include <poll.h>
#include <sched.h>
#include <time.h>
#endif

#include <weave/postgres.hpp>
#include <weave/runtime.hpp>
#include <weave/channel.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <libpq-fe.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
static constexpr weave::u64 checksum_seed = 14695981039346656037ULL;

struct Configuration {
  std::string library, workload, ca;
  weave::pg::Options database;
  std::size_t workers = 1, connections = 32, iterations = 1000, batch = 32, warmup = 32;
  std::vector<int> cpus;
  std::string payload = std::string(256, 'x');

  const char *sql() const noexcept
  {
    if (workload == "rows")
      return "SELECT i, repeat('x', 256) FROM generate_series(1, 128) i";
    return workload == "simple" ? "SELECT 42::int" : "SELECT $1::int + 1";
  }
};

struct Metrics {
  std::vector<double> latency;
  weave::u64 checksum = checksum_seed;
  std::size_t operations = 0;

  void consume(std::string_view value) noexcept
  {
    for (auto byte : value) {
      checksum ^= static_cast<unsigned char>(byte);
      checksum *= 1099511628211ULL;
    }
    checksum ^= value.size();
    checksum *= 1099511628211ULL;
  }
};

struct Control {
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t ready = 0, done = 0;
  bool start = false, cleanup = false;
  std::atomic<bool> failed{false};
  weave::Channel<bool> start_tasks, close_tasks;

  explicit Control(std::size_t connections) : start_tasks(connections), close_tasks(connections)
  {
  }

  void advance(std::size_t &counter, std::size_t count = 1)
  {
    std::lock_guard lock(mutex);
    counter += count;
    changed.notify_all();
  }

  void fail() noexcept
  {
    std::lock_guard lock(mutex);
    failed.store(true, std::memory_order_release);
    changed.notify_all();
  }
};

static double process_cpu()
{
#if defined(_WIN32)
  FILETIME creation, exit, kernel, user;
  if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
    std::abort();
  ULARGE_INTEGER kernel_time{}, user_time{};
  kernel_time.LowPart = kernel.dwLowDateTime;
  kernel_time.HighPart = kernel.dwHighDateTime;
  user_time.LowPart = user.dwLowDateTime;
  user_time.HighPart = user.dwHighDateTime;
  return static_cast<double>(kernel_time.QuadPart + user_time.QuadPart) / 10000000;
#else
  timespec value{};
  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0)
    std::abort();
  return static_cast<double>(value.tv_sec) + static_cast<double>(value.tv_nsec) / 1000000000;
#endif
}

static weave::u64 process_cycles()
{
#if defined(_WIN32)
  ULONG64 cycles = 0;
  if (!QueryProcessCycleTime(GetCurrentProcess(), &cycles))
    std::abort();
  return cycles;
#else
  return 0;
#endif
}

static std::vector<std::vector<int>> cpu_topology()
{
  std::vector<std::vector<int>> cores;
#if defined(_WIN32)
  auto topology = experiment::cpu_topology();
  for (auto mask : topology.cores) {
    std::vector<int> cpus;
    mask &= topology.allowed;
    while (mask) {
      int cpu = std::countr_zero(mask);
      cpus.push_back(cpu);
      mask &= mask - 1;
    }
    if (!cpus.empty())
      cores.push_back(std::move(cpus));
  }
#else
  cpu_set_t allowed;
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
    return {};
  std::map<std::pair<int, int>, std::vector<int>> groups;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &allowed))
      continue;
    auto root = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
    int package = -1, core = -1;
    std::ifstream package_file(root + "physical_package_id"), core_file(root + "core_id");
    if (!(package_file >> package) || !(core_file >> core))
      return {};
    groups[{package, core}].push_back(cpu);
  }
  for (auto &[identity, cpus] : groups)
    cores.push_back(std::move(cpus));
#endif
  return cores;
}

static bool set_affinity(const Configuration &configuration)
{
  auto topology = cpu_topology();
  std::vector<bool> used(topology.size());
  for (int cpu : configuration.cpus) {
    auto found = std::ranges::find_if(topology, [cpu](const auto &core) {
      return std::ranges::find(core, cpu) != core.end();
    });
    if (found == topology.end() || used[found - topology.begin()])
      return false;
    used[found - topology.begin()] = true;
  }
#if defined(_WIN32)
  DWORD_PTR mask = 0;
  for (int cpu : configuration.cpus)
    mask |= DWORD_PTR{1} << cpu;
  return SetProcessAffinityMask(GetCurrentProcess(), mask) != 0;
#else
  cpu_set_t selected;
  CPU_ZERO(&selected);
  for (int cpu : configuration.cpus)
    CPU_SET(cpu, &selected);
  return sched_setaffinity(0, sizeof(selected), &selected) == 0;
#endif
}

static bool valid_value(const Configuration &configuration, std::size_t row, int column, std::string_view value)
{
  if (configuration.workload != "rows")
    return value == "42";
  if (column == 1)
    return value == configuration.payload;
  std::size_t integer = 0;
  auto parsed = std::from_chars(value.data(), value.data() + value.size(), integer);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && integer == row + 1;
}

static bool consume(Metrics &metrics, const Configuration &configuration, const weave::pg::ResultSet &result)
{
  std::size_t rows = configuration.workload == "rows" ? 128 : 1;
  std::size_t columns = configuration.workload == "rows" ? 2 : 1;
  if (result.rows.size() != rows || result.columns.size() != columns)
    return false;
  for (std::size_t row = 0; row < rows; ++row) {
    if (result.rows[row].size() != columns)
      return false;
    for (std::size_t column = 0; column < columns; ++column) {
      const auto &value = result.rows[row][column];
      if (value.is_null() || !valid_value(configuration, row, static_cast<int>(column), value.bytes()))
        return false;
      metrics.consume(value.bytes());
    }
  }
  return true;
}

static bool consume(Metrics &metrics, const Configuration &configuration, PGresult *result)
{
  int rows = configuration.workload == "rows" ? 128 : 1;
  int columns = configuration.workload == "rows" ? 2 : 1;
  if (PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) != rows || PQnfields(result) != columns)
    return false;
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < columns; ++column) {
      auto value = std::string_view{
        PQgetvalue(result, row, column),
        static_cast<std::size_t>(PQgetlength(result, row, column))};
      if (PQgetisnull(result, row, column) || !valid_value(configuration, row, column, value))
        return false;
      metrics.consume(value);
    }
  }
  return true;
}

static weave::Task<void> weave_operation(
  weave::pg::Connection &connection,
  const Configuration &configuration,
  const std::vector<weave::pg::Parameter> &parameters,
  const std::vector<weave::pg::Command> &commands,
  Metrics &metrics)
{
  if (configuration.workload == "batch") {
    auto outcomes = co_await connection.batch(commands);
    if (outcomes.size() != configuration.batch)
      co_await weave::fail(std::errc::bad_message);
    for (const auto &outcome : outcomes) {
      if (!outcome.result || outcome.aborted || !outcome.error.fields.empty() ||
        !consume(metrics, configuration, *outcome.result))
        co_await weave::fail(std::errc::bad_message);
    }
  } else if (configuration.workload == "prepared") {
    auto result = co_await connection.execute_prepared("bench", parameters);
    if (!consume(metrics, configuration, result))
      co_await weave::fail(std::errc::bad_message);
  } else {
    auto results = co_await connection.query(configuration.sql());
    if (results.size() != 1 || !consume(metrics, configuration, results.front()))
      co_await weave::fail(std::errc::bad_message);
  }
}

static weave::Task<void> weave_client(const Configuration &configuration, Control &control, Metrics &metrics)
{
  auto connection = co_await weave::pg::connect(configuration.database);
  if (connection.protocol_version() != weave::pg::ProtocolVersion::v30)
    co_await weave::fail(std::errc::bad_message);
  std::vector<weave::pg::Parameter> parameters{{"41", 23}};
  std::vector<weave::pg::Command> commands(configuration.batch, {configuration.sql(), parameters});
  if (configuration.workload == "prepared")
    co_await connection.prepare("bench", configuration.sql(), {23});

  Metrics warmup;
  for (std::size_t index = 0; index < configuration.warmup; ++index)
    co_await weave_operation(connection, configuration, parameters, commands, warmup);
  control.advance(control.ready);
  if (!co_await control.start_tasks.receive())
    co_await weave::fail(std::errc::operation_canceled);

  auto operations = configuration.workload == "batch" ? configuration.batch : 1;
  for (std::size_t index = 0; index < configuration.iterations; ++index) {
    auto start = Clock::now();
    co_await weave_operation(connection, configuration, parameters, commands, metrics);
    metrics.latency.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
    metrics.operations += operations;
  }
  control.advance(control.done);
  if (!co_await control.close_tasks.receive())
    co_await weave::fail(std::errc::operation_canceled);
  co_await connection.finish();
}

using PqConnection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using PqResult = std::unique_ptr<PGresult, decltype(&PQclear)>;

static PqConnection pq_connect(const Configuration &configuration)
{
  auto port = std::to_string(configuration.database.port);
  const char *keys[] = {
    "host",
    "port",
    "user",
    "dbname",
    "password",
    "application_name",
    "sslmode",
    "sslrootcert",
    "channel_binding",
    "gssencmode",
    "sslcertmode",
    "ssl_min_protocol_version",
    "ssl_max_protocol_version",
    "min_protocol_version",
    "max_protocol_version",
    "require_auth",
    "connect_timeout",
    nullptr};
  const char *values[] = {
    configuration.database.host.c_str(),
    port.c_str(),
    "weave",
    "postgres",
    configuration.database.password.c_str(),
    "weave-concurrent",
    configuration.database.plaintext ? "disable" : "verify-full",
    configuration.ca.c_str(),
    configuration.database.plaintext ? "disable" : "require",
    "disable",
    "disable",
    "TLSv1.3",
    "TLSv1.3",
    "3.0",
    "3.0",
    "scram-sha-256",
    "15",
    nullptr};
  return PqConnection{PQconnectdbParams(keys, values, 0), PQfinish};
}

struct PqSession {
  PqConnection connection{nullptr, PQfinish};
  Metrics *metrics = nullptr;
  Clock::time_point submitted;
  std::size_t completed = 0, results = 0, delimiters = 0;
  bool writing = false;
};

static bool pq_send(PqSession &session, const Configuration &configuration)
{
  const char *values[] = {"41"};
  const Oid types[] = {23};
  session.submitted = Clock::now();
  session.results = 0;
  session.delimiters = 0;
  auto connection = session.connection.get();
  if (configuration.workload == "batch") {
    for (std::size_t query = 0; query < configuration.batch; ++query) {
      if (!PQsendQueryParams(connection, configuration.sql(), 1, types, values, nullptr, nullptr, 0))
        return false;
    }
    if (!PQpipelineSync(connection))
      return false;
  } else if (configuration.workload == "prepared") {
    if (!PQsendQueryPrepared(connection, "bench", 1, values, nullptr, nullptr, 0))
      return false;
  } else if (!PQsendQuery(connection, configuration.sql())) {
    return false;
  }
  int flush = PQflush(connection);
  session.writing = flush == 1;
  return flush >= 0;
}

#if defined(_WIN32)
using Poll = WSAPOLLFD;
#else
using Poll = pollfd;
#endif

static bool pq_phase(
  std::vector<PqSession> &sessions,
  std::vector<Poll> &polls,
  std::vector<std::size_t> &indices,
  const Configuration &configuration,
  Control &control,
  std::size_t iterations,
  bool measured)
{
  auto deadline = Clock::now() + std::chrono::seconds{90};
  for (auto &session : sessions) {
    session.completed = 0;
    if (!pq_send(session, configuration))
      return false;
  }

  std::size_t remaining = sessions.size();
  while (remaining) {
    if (control.failed.load(std::memory_order_acquire) || Clock::now() >= deadline)
      return false;
    polls.clear();
    indices.clear();
    for (std::size_t index = 0; index < sessions.size(); ++index) {
      const auto &session = sessions[index];
      if (session.completed == iterations)
        continue;
      Poll descriptor{};
#if defined(_WIN32)
      descriptor.fd = static_cast<SOCKET>(PQsocket(session.connection.get()));
#else
      descriptor.fd = PQsocket(session.connection.get());
#endif
      descriptor.events = POLLIN | (session.writing ? POLLOUT : 0);
      polls.push_back(descriptor);
      indices.push_back(index);
    }
#if defined(_WIN32)
    int ready = WSAPoll(polls.data(), static_cast<ULONG>(polls.size()), 1000);
#else
    int ready = poll(polls.data(), polls.size(), 1000);
    if (ready < 0 && errno == EINTR)
      continue;
#endif
    if (ready < 0)
      return false;

    for (std::size_t index = 0; index < polls.size(); ++index) {
      auto &descriptor = polls[index];
      if (!descriptor.revents)
        continue;
      auto &session = sessions[indices[index]];
      auto connection = session.connection.get();
      if (descriptor.revents & POLLNVAL)
        return false;
      if (descriptor.revents & (POLLIN | POLLERR | POLLHUP)) {
        if (!PQconsumeInput(connection))
          return false;
      }
      if (session.writing && (descriptor.revents & (POLLIN | POLLOUT))) {
        int flush = PQflush(connection);
        if (flush < 0)
          return false;
        session.writing = flush == 1;
      }

      bool completed = false;
      while (!PQisBusy(connection)) {
        PqResult result{PQgetResult(connection), PQclear};
        if (!result) {
          if (PQstatus(connection) != CONNECTION_OK)
            return false;
          if (configuration.workload == "batch") {
            if (++session.delimiters > configuration.batch)
              return false;
            continue;
          }
          completed = true;
        } else if (PQresultStatus(result.get()) == PGRES_PIPELINE_SYNC) {
          if (configuration.workload != "batch")
            return false;
          PqResult extra{PQgetResult(connection), PQclear};
          if (extra)
            return false;
          completed = true;
        } else {
          if (!consume(*session.metrics, configuration, result.get()))
            return false;
          ++session.results;
        }
        if (completed)
          break;
      }
      if (!completed)
        continue;
      auto operations = configuration.workload == "batch" ? configuration.batch : 1;
      if (session.results != operations || session.writing)
        return false;
      if (measured) {
        session.metrics->latency.push_back(
          std::chrono::duration<double, std::micro>(Clock::now() - session.submitted).count());
        session.metrics->operations += operations;
      }
      if (++session.completed == iterations)
        --remaining;
      else if (!pq_send(session, configuration))
        return false;
    }
  }
  return true;
}

static bool pq_worker(
  const Configuration &configuration,
  Control &control,
  std::vector<Metrics> &metrics,
  std::size_t worker)
{
  std::vector<PqSession> sessions;
  for (std::size_t index = worker; index < configuration.connections; index += configuration.workers) {
    if (control.failed.load())
      return false;
    auto connection = pq_connect(configuration);
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK ||
      PQfullProtocolVersion(connection.get()) != 30000) {
      std::fprintf(
        stderr,
        "libpq setup: %s (protocol %d)\n",
        connection ? PQerrorMessage(connection.get()) : "allocation failed",
        connection ? PQfullProtocolVersion(connection.get()) : 0);
      return false;
    }
    if (configuration.workload == "prepared") {
      const Oid types[] = {23};
      PqResult prepared{PQprepare(connection.get(), "bench", configuration.sql(), 1, types), PQclear};
      if (!prepared || PQresultStatus(prepared.get()) != PGRES_COMMAND_OK)
        return false;
    }
    if (PQsetnonblocking(connection.get(), 1) != 0 ||
      (configuration.workload == "batch" && !PQenterPipelineMode(connection.get())))
      return false;
    sessions.push_back({std::move(connection), &metrics[index]});
  }
  std::vector<Poll> polls;
  std::vector<std::size_t> indices;
  polls.reserve(sessions.size());
  indices.reserve(sessions.size());
  if (!pq_phase(sessions, polls, indices, configuration, control, configuration.warmup, false))
    return false;
  for (auto &session : sessions)
    session.metrics->checksum = checksum_seed;
  control.advance(control.ready, sessions.size());
  {
    std::unique_lock lock(control.mutex);
    control.changed.wait(lock, [&] {
      return control.start || control.failed.load();
    });
  }
  if (control.failed.load() ||
    !pq_phase(sessions, polls, indices, configuration, control, configuration.iterations, true))
    return false;
  control.advance(control.done, sessions.size());
  {
    std::unique_lock lock(control.mutex);
    control.changed.wait(lock, [&] {
      return control.cleanup || control.failed.load();
    });
  }
  return !control.failed.load();
}

static bool wait_count(Control &control, const std::size_t &counter, std::size_t expected)
{
  std::unique_lock lock(control.mutex);
  bool changed = control.changed.wait_for(lock, std::chrono::seconds{120}, [&] {
    return counter == expected || control.failed.load();
  });
  return changed && !control.failed.load();
}

static void open_gate(Control &control, bool start, const Configuration &configuration)
{
  {
    std::lock_guard lock(control.mutex);
    (start ? control.start : control.cleanup) = true;
    control.changed.notify_all();
  }
  if (configuration.library == "libpq")
    return;
  auto &channel = start ? control.start_tasks : control.close_tasks;
  for (std::size_t index = 0; index < configuration.connections; ++index) {
    bool token = true;
    if (!channel.try_send(token))
      std::abort();
  }
}

struct Measurement {
  double seconds = 0, cpu = 0;
  weave::u64 cycles = 0;
};

static weave::Result<Measurement> measure(Control &control, const Configuration &configuration)
{
  bool successful = wait_count(control, control.ready, configuration.connections);
  Measurement measurement;
  if (successful) {
    auto cpu = process_cpu();
    auto cycles = process_cycles();
    auto start = Clock::now();
    open_gate(control, true, configuration);
    successful = wait_count(control, control.done, configuration.connections);
    measurement.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    measurement.cpu = process_cpu() - cpu;
    measurement.cycles = process_cycles() - cycles;
  }
  if (!successful)
    control.fail();
  open_gate(control, false, configuration);
  if (!successful) {
    control.start_tasks.close();
    control.close_tasks.close();
    return std::unexpected(std::make_error_code(std::errc::io_error));
  }
  return measurement;
}

static weave::Result<Measurement> drive_weave(
  const Configuration &configuration,
  Control &control,
  std::vector<Metrics> &metrics)
{
  auto runtime = weave::Runtime::create(
    {.workers = configuration.workers,
      .scheduler = configuration.library == "weave-stealing" ? weave::Scheduler::work_stealing
                                                             : weave::Scheduler::worker_affine});
  if (!runtime)
    return std::unexpected(runtime.error());
  std::vector<weave::JoinHandle<void>> tasks;
  for (std::size_t index = 0; index < configuration.connections; ++index) {
    auto task = weave_client(configuration, control, metrics[index]).on_error([&](weave::Error error) noexcept {
      std::fprintf(stderr, "Weave client failed: %s\n", error.message().c_str());
      control.fail();
    });
    auto job = runtime->spawn(std::move(task));
    if (!job) {
      control.fail();
      break;
    }
    tasks.push_back(std::move(*job));
  }

  auto measurement = measure(control, configuration);
  if (!measurement)
    runtime->shutdown();
  for (auto &job : tasks) {
    if (auto result = std::move(job).get(); !result)
      measurement = std::unexpected(result.error());
  }
  return measurement;
}

static weave::Result<Measurement> drive_pq(
  const Configuration &configuration,
  Control &control,
  std::vector<Metrics> &metrics)
{
  std::vector<std::jthread> threads;
  for (std::size_t worker = 0; worker < configuration.workers; ++worker) {
    threads.emplace_back([&, worker] {
      if (!pq_worker(configuration, control, metrics, worker)) {
        std::fprintf(stderr, "libpq worker %zu failed\n", worker);
        control.fail();
      }
    });
  }
  auto measurement = measure(control, configuration);
  threads.clear();
  return measurement;
}

static int compare(const Configuration &configuration)
{
  Control control(configuration.connections);
  std::vector<Metrics> metrics(configuration.connections);
  for (auto &metric : metrics)
    metric.latency.reserve(configuration.iterations);
  auto measurement = configuration.library == "libpq" ? drive_pq(configuration, control, metrics)
                                                      : drive_weave(configuration, control, metrics);
  if (!measurement || control.failed.load()) {
    std::fprintf(stderr, "Concurrent comparison failed during setup, warmup, measurement or cleanup\n");
    return 1;
  }

  std::vector<double> latency;
  latency.reserve(configuration.iterations * configuration.connections);
  weave::u64 checksum = checksum_seed;
  std::size_t operations = 0;
  for (const auto &metric : metrics) {
    if (metric.latency.size() != configuration.iterations)
      return 1;
    latency.insert(latency.end(), metric.latency.begin(), metric.latency.end());
    checksum ^= metric.checksum;
    checksum *= 1099511628211ULL;
    operations += metric.operations;
  }
  std::ranges::sort(latency);
  auto percentile = [&](double fraction) {
    return latency[static_cast<std::size_t>(fraction * (latency.size() - 1))];
  };
  std::printf(
    "{\"operations\":%zu,\"roundtrips\":%zu,\"seconds\":%.9f,\"cpu_seconds\":%.9f,\"ops_per_second\":%.3f,"
    "\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,\"checksum\":%llu,\"libpq_version\":%d,\"cpu_cycles\":%llu}\n",
    operations,
    latency.size(),
    measurement->seconds,
    measurement->cpu,
    static_cast<double>(operations) / measurement->seconds,
    percentile(0.50),
    percentile(0.95),
    percentile(0.99),
    static_cast<unsigned long long>(checksum),
    PQlibVersion(),
    static_cast<unsigned long long>(measurement->cycles));
  return 0;
}

int main(int argc, char **argv)
{
  if (argc == 2 && std::string_view{argv[1]} == "--topology") {
    auto cores = cpu_topology();
    if (cores.empty())
      return 1;
    std::printf("{\"cores\":[");
    for (std::size_t index = 0; index < cores.size(); ++index) {
      std::printf("%s[", index ? "," : "");
      for (std::size_t cpu = 0; cpu < cores[index].size(); ++cpu)
        std::printf("%s%d", cpu ? "," : "", cores[index][cpu]);
      std::printf("]");
    }
    std::printf("],\"libpq_version\":%d}\n", PQlibVersion());
    return 0;
  }
  if (argc != 12 || PQlibVersion() < 180000) {
    std::fprintf(
      stderr,
      "Usage: concurrent weave-affine|weave-stealing|libpq simple|prepared|batch|rows "
      "workers connections iterations batch host port ca-or-plain cpus warmup (libpq 18+)\n");
    return 2;
  }
  Configuration configuration;
  configuration.library = argv[1];
  configuration.workload = argv[2];
  auto workers = weave::pg::Value{{argv[3]}}.integer<std::size_t>();
  auto connections = weave::pg::Value{{argv[4]}}.integer<std::size_t>();
  auto iterations = weave::pg::Value{{argv[5]}}.integer<std::size_t>();
  auto batch = weave::pg::Value{{argv[6]}}.integer<std::size_t>();
  auto warmup = weave::pg::Value{{argv[11]}}.integer<std::size_t>();
  auto port = weave::parse_port(argv[8]);
  const std::array libraries{"weave-affine", "weave-stealing", "libpq"};
  const std::array workloads{"simple", "prepared", "batch", "rows"};
  if (!workers || !*workers || *workers > 32 || !connections || *connections < *workers || *connections > 256 ||
    !iterations || !*iterations || *iterations > 2000000 / *connections || !batch || !*batch || *batch > 256 ||
    !warmup || !*warmup || *warmup > 10000 || !port || *port == 0 ||
    std::ranges::find(libraries, configuration.library) == libraries.end() ||
    std::ranges::find(workloads, configuration.workload) == workloads.end())
    return 2;
  configuration.workers = *workers;
  configuration.connections = *connections;
  configuration.iterations = *iterations;
  configuration.batch = *batch;
  configuration.warmup = *warmup;
  configuration.database.host = argv[7];
  configuration.database.port = *port;
  configuration.database.user = "weave";
  configuration.database.database = "postgres";
  configuration.database.application_name = "weave-concurrent";
  configuration.database.min_protocol = weave::pg::ProtocolVersion::v30;
  configuration.database.max_protocol = weave::pg::ProtocolVersion::v30;
  configuration.database.authentication.methods = {weave::pg::Authentication::scram_sha256};
  auto password = std::getenv("WEAVE_PG_PASSWORD");
  if (!password)
    return 2;
  configuration.database.password = password;
  configuration.database.plaintext = std::string_view{argv[9]} == "plain";
  if (!configuration.database.plaintext) {
    configuration.ca = argv[9];
    auto credentials = weave::TlsContext::client(
      {.ca_file = configuration.ca, .min_version = weave::TlsVersion::tls13});
    if (!credentials)
      return weave::report_error(credentials.error());
    configuration.database.tls = *credentials;
    configuration.database.channel_binding = weave::pg::ChannelBinding::require;
  }
  std::string_view cpus = argv[10];
  while (!cpus.empty()) {
    auto comma = cpus.find(',');
    auto parsed = weave::pg::Value{{std::string{cpus.substr(0, comma)}}}.integer<int>();
    if (!parsed || *parsed < 0)
      return 2;
    configuration.cpus.push_back(*parsed);
    if (comma == std::string_view::npos)
      break;
    cpus.remove_prefix(comma + 1);
    if (cpus.empty())
      return 2;
  }
  if (configuration.cpus.size() != configuration.workers || !set_affinity(configuration))
    return 2;
  return compare(configuration);
}
