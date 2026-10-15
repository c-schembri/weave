#include <weave/mysql.hpp>
#include <weave/runtime.hpp>
#include <weave/channel.hpp>
#include <weave/port.hpp>
#include <mysql.h>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

static bool diagnostics = false;
static bool affine_diagnostics = false;
static bool native_diagnostics = false;
static constexpr char tls_version[] = "TLSv1.3";
static constexpr char tls_cipher[] = "TLS_AES_256_GCM_SHA384";
static constexpr std::string_view tls_status_query = "SHOW SESSION STATUS LIKE 'Ssl_version'";
#if !defined(_WIN32)
static bool cpu_phases = false;
static unsigned long long cpu_phase_begin = 0;
#endif

struct Samples {
  std::vector<double> latency;
  double seconds = 0;
};

struct Workload {
  const char *name;
  std::string sql;
  std::size_t rows;
  std::size_t bytes = 0;
  bool stream = false;
  bool prepared = false;
  bool batch = false;
  bool reset = false;
  bool connect = false;
  int workers = 1;
  bool affine = false;
  bool shared = false;
};

static void require(bool valid, const char *message)
{
  if (!valid) {
    std::fprintf(stderr, "Comparison failed: %s\n", message);
    std::abort();
  }
}

static double cpu_seconds()
{
#if defined(_WIN32)
  FILETIME created{}, exited{}, kernel{}, user{};
  require(GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != 0, "GetProcessTimes");
  auto ticks = [](FILETIME value) {
    return (static_cast<unsigned long long>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
  };
  return (ticks(kernel) + ticks(user)) / 10000000.0;
#else
  timespec value{};
  require(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) == 0, "process CPU clock");
  return value.tv_sec + value.tv_nsec / 1000000000.0;
#endif
}

static unsigned long long cpu_cycles()
{
#if defined(_WIN32)
  unsigned long long cycles = 0;
  require(QueryProcessCycleTime(GetCurrentProcess(), &cycles) != 0, "process CPU cycles");
  return cycles;
#else
  return 0;
#endif
}

#if !defined(_WIN32)
static unsigned long long diagnostic_nanoseconds()
{
  timespec value{};
  require(clock_gettime(CLOCK_MONOTONIC, &value) == 0, "diagnostic monotonic clock");
  return static_cast<unsigned long long>(value.tv_sec) * 1000000000ULL + value.tv_nsec;
}
#endif

static void consume(const weave::mysql::ResultSet &result, const Workload &work)
{
  require(result.rows.size() == work.rows, "Weave row count");
  for (const auto &row : result.rows) {
    if (work.bytes) {
      auto data = row[0].bytes();
      require(data && data->size() == work.bytes && data->front() == 'x' && data->back() == 'x', "Weave payload");
    } else {
      require(row[0].as<long long>() == 42, "Weave numeric result");
    }
  }
}

static weave::Task<Samples> weave_worker(
  weave::mysql::Options options,
  Workload work,
  std::chrono::milliseconds duration,
  std::atomic<int> &ready,
  weave::Channel<int> &start)
{
#if defined(WEAVE_PROFILE_RUNTIME)
  auto *io_context = weave::detail::current_context;
  std::size_t foreign_iterations = 0;
#endif
  auto db = co_await weave::mysql::connect(options);
  {
    auto startup = co_await db.query_borrowed(tls_status_query);
    require(
      startup.size() == 1 && startup[0].rows.size() == 1 && startup[0].rows[0].size() == 2,
      "Weave TLS version status shape");
    auto variable = startup[0].rows[0][0].bytes();
    auto version = startup[0].rows[0][1].bytes();
    require(variable && *variable == "Ssl_version" && version && *version == tls_version, "Weave TLS version status");
    auto encryption = db.tls_info();
    require(encryption && *encryption, "Weave negotiated TLS metadata");
    const auto &info = **encryption;
    require(
      info.version == weave::TlsVersion::tls13 && info.cipher == tls_cipher && info.certificate_verified &&
        info.hostname_verified && !info.compression,
      "Weave matched verified TLS policy");
    std::fprintf(
      stderr,
      "Negotiated TLS weave: version=%s cipher=%s verified=certificate+hostname compression=none\n",
      tls_version,
      info.cipher.c_str());
  }
  std::optional<weave::mysql::Statement> prepared;
  if (work.prepared)
    prepared.emplace(co_await db.prepare(work.sql));
  const std::array<weave::mysql::ParameterView, 1> parameters{42};
  Samples sample;
  sample.latency.reserve(100000);
  ready.fetch_add(1, std::memory_order_release);
  co_await start.receive();
#if defined(WEAVE_PROFILE_RUNTIME)
  auto *context = weave::detail::current_context;
  const auto io_before = context->metrics();
  const auto frames_before = weave::detail::frame_metrics;
#endif
  const auto begin = Clock::now();
  const auto deadline = begin + duration;
  do {
#if defined(WEAVE_PROFILE_RUNTIME)
    if (diagnostics && weave::detail::current_context != io_context)
      ++foreign_iterations;
#endif
    const auto operation = Clock::now();
    if (work.connect) {
      auto connection = co_await weave::mysql::connect(options);
      consume(co_await connection.execute_borrowed(work.sql), work);
      co_await connection.disconnect();
    } else if (work.reset) {
      co_await db.reset();
      consume(co_await db.execute_borrowed(work.sql), work);
    } else if (work.batch) {
      std::vector<weave::mysql::Command> commands(4, weave::mysql::Command{work.sql});
      auto outcomes = co_await db.batch(std::move(commands));
      require(outcomes.size() == 4, "Weave batch count");
      for (const auto &outcome : outcomes) {
        require(!outcome.failure && outcome.results.size() == 1, "Weave batch success");
        consume(outcome.results[0], work);
      }
    } else if (work.stream) {
      auto stream = co_await db.stream_borrowed(work.sql);
      std::size_t rows = 0;
      while (auto row = co_await stream.next()) {
        require((*row)[0].as<long long>() == 42, "Weave stream value");
        ++rows;
      }
      require(rows == work.rows, "Weave stream count");
    } else if (prepared) {
      consume(co_await prepared->execute_borrowed(parameters), work);
    } else {
      auto results = co_await db.query_borrowed(work.sql);
      require(results.size() == (work.name == std::string_view{"multi"} ? 4 : 1), "Weave result count");
      for (const auto &result : results)
        consume(result, work);
    }
    sample.latency.push_back(std::chrono::duration<double, std::micro>(Clock::now() - operation).count());
  } while (Clock::now() < deadline);
  sample.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
#if defined(WEAVE_PROFILE_RUNTIME)
  if (diagnostics) {
    std::fprintf(
      stderr,
      "Diagnostic placement %s/%d: io_context=%p operations=%zu foreign_iterations=%zu\n",
      work.name,
      work.workers,
      static_cast<void *>(io_context),
      sample.latency.size(),
      foreign_iterations);
  }
  // Thread-local counters only describe this root in the explicitly affine diagnostic run.
  if (affine_diagnostics) {
    const auto io = context->metrics();
    const auto frames = weave::detail::frame_metrics;
    const auto operations = static_cast<double>(sample.latency.size());
    std::fprintf(
      stderr,
      "Diagnostic %s/%d: operations=%zu reads/op=%.3f writes/op=%.3f frames/op=%.3f "
      "frame_heap/op=%.3f frame_bytes/op=%.0f frame_heap_bytes/op=%.0f dequeue/op=%.3f fairness/op=%.3f\n",
      work.name,
      work.workers,
      sample.latency.size(),
      (io.read_calls - io_before.read_calls) / operations,
      (io.write_calls - io_before.write_calls) / operations,
      (frames.allocations - frames_before.allocations) / operations,
      (frames.heap_allocations - frames_before.heap_allocations) / operations,
      (frames.bytes - frames_before.bytes) / operations,
      (frames.heap_bytes - frames_before.heap_bytes) / operations,
      (io.dequeue_calls - io_before.dequeue_calls) / operations,
      (io.fairness_posts - io_before.fairness_posts) / operations);
  }
#endif
  co_return sample;
}

struct Native {
  MYSQL *db = nullptr;

  Native(unsigned port, const char *ca, const char *password)
  {
    db = mysql_init(nullptr);
    require(db != nullptr, "mysql_init");
    const auto ssl = SSL_MODE_VERIFY_IDENTITY;
    require(mysql_options(db, MYSQL_OPT_SSL_MODE, &ssl) == 0, "native TLS mode");
    require(mysql_options(db, MYSQL_OPT_SSL_CA, ca) == 0, "native CA");
    require(mysql_options(db, MYSQL_OPT_TLS_VERSION, tls_version) == 0, "native TLS version policy");
    require(mysql_options(db, MYSQL_OPT_TLS_CIPHERSUITES, tls_cipher) == 0, "native TLS cipher policy");
    require(mysql_options(db, MYSQL_SET_CHARSET_NAME, "utf8mb4") == 0, "native charset");
    require(
      mysql_real_connect(
        db,
        "127.0.0.1",
        "weave",
        password,
        "weave_test",
        port,
        nullptr,
        CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS) != nullptr,
      mysql_error(db));
    require(mysql_get_ssl_cipher(db) != nullptr, "native encrypted session");
  }

  ~Native()
  {
    mysql_close(db);
  }
};

static void native_query(MYSQL *db, const Workload &work)
{
  require(mysql_real_query(db, work.sql.data(), static_cast<unsigned long>(work.sql.size())) == 0, mysql_error(db));
  int results = 0;
  int next = 0;
  do {
    auto result = work.stream ? mysql_use_result(db) : mysql_store_result(db);
    require(result != nullptr, mysql_error(db));
    std::size_t count = 0;
    while (auto row = mysql_fetch_row(result)) {
      auto lengths = mysql_fetch_lengths(result);
      require(row[0] != nullptr && lengths != nullptr, "native nonnull value");
      if (work.bytes) {
        require(lengths[0] == work.bytes && row[0][0] == 'x' && row[0][work.bytes - 1] == 'x', "native payload");
      } else {
        long long value = 0;
        auto parsed = std::from_chars(row[0], row[0] + lengths[0], value);
        require(parsed.ec == std::errc{} && parsed.ptr == row[0] + lengths[0] && value == 42, "native numeric result");
      }
      ++count;
    }
    require(mysql_errno(db) == 0 && count == work.rows, "native row count");
    mysql_free_result(result);
    ++results;
    next = mysql_next_result(db);
  } while (next == 0);
  require(next == -1 && results == (work.name == std::string_view{"multi"} ? 4 : 1), "native result count");
}

static Samples native_worker(
  unsigned port,
  const char *ca,
  const char *password,
  const Workload &work,
  std::chrono::milliseconds duration,
  std::atomic<int> &ready,
  std::barrier<> &start)
{
  require(mysql_thread_init() == 0, "mysql_thread_init");
  Samples sample;
  {
    Native connection{port, ca, password};
    {
      const auto *cipher = mysql_get_ssl_cipher(connection.db);
      require(cipher && std::string_view{cipher} == tls_cipher, "native matched TLS cipher");
      require(
        mysql_real_query(connection.db, tls_status_query.data(), static_cast<unsigned long>(tls_status_query.size())) ==
          0,
        mysql_error(connection.db));
      auto *result = mysql_store_result(connection.db);
      require(result && mysql_num_fields(result) == 2, "native TLS version status shape");
      auto row = mysql_fetch_row(result);
      require(row && row[0] && row[1], "native TLS version status row");
      auto lengths = mysql_fetch_lengths(result);
      require(
        lengths && std::string_view{row[0], lengths[0]} == "Ssl_version" &&
          std::string_view{row[1], lengths[1]} == tls_version,
        "native TLS version status");
      require(mysql_fetch_row(result) == nullptr && mysql_errno(connection.db) == 0, "native TLS version status count");
      mysql_free_result(result);
      require(mysql_next_result(connection.db) == -1, "native TLS version status drain");
      std::fprintf(stderr, "Negotiated TLS native: version=%s cipher=%s verified=identity\n", tls_version, cipher);
    }
    if (native_diagnostics) {
      std::fprintf(
        stderr,
        "Diagnostic native %s/%d: client_flags=0x%lx deprecate_eof=%d optional_metadata=%d\n",
        work.name,
        work.workers,
        connection.db->client_flag,
        (connection.db->client_flag & CLIENT_DEPRECATE_EOF) != 0,
        (connection.db->client_flag & CLIENT_OPTIONAL_RESULTSET_METADATA) != 0);
    }
    MYSQL_STMT *statement = nullptr;
    long long parameter = 42;
    long long value = 0;
    bool is_null = false;
    bool error = false;
    MYSQL_BIND input{};
    MYSQL_BIND output{};
    if (work.prepared) {
      statement = mysql_stmt_init(connection.db);
      require(
        statement && mysql_stmt_prepare(statement, work.sql.data(), static_cast<unsigned long>(work.sql.size())) == 0,
        mysql_error(connection.db));
      input.buffer_type = MYSQL_TYPE_LONGLONG;
      input.buffer = &parameter;
      output.buffer_type = MYSQL_TYPE_LONGLONG;
      output.buffer = &value;
      output.is_null = &is_null;
      output.error = &error;
      require(
        mysql_stmt_bind_param(statement, &input) == 0 && mysql_stmt_bind_result(statement, &output) == 0,
        "native bind");
    }
    sample.latency.reserve(100000);
    ready.fetch_add(1, std::memory_order_release);
    start.arrive_and_wait();
    const auto begin = Clock::now();
    const auto deadline = begin + duration;
    do {
      const auto operation = Clock::now();
      if (work.connect) {
        Native client{port, ca, password};
        native_query(client.db, work);
      } else if (work.reset) {
        require(mysql_reset_connection(connection.db) == 0, "native reset");
        require(
          mysql_set_character_set(connection.db, "utf8mb4") == 0 && mysql_select_db(connection.db, "weave_test") == 0,
          "native reset restoration");
        native_query(connection.db, work);
      } else if (statement) {
        require(
          mysql_stmt_execute(statement) == 0 && mysql_stmt_store_result(statement) == 0,
          "native prepared execution");
        require(mysql_stmt_fetch(statement) == 0 && !is_null && !error && value == 42, "native binary result");
        require(mysql_stmt_fetch(statement) == MYSQL_NO_DATA, "native binary EOF");
        mysql_stmt_free_result(statement);
      } else {
        const auto commands = work.batch ? 4 : 1;
        for (int index = 0; index < commands; ++index)
          native_query(connection.db, work);
      }
      sample.latency.push_back(std::chrono::duration<double, std::micro>(Clock::now() - operation).count());
    } while (Clock::now() < deadline);
    sample.seconds = std::chrono::duration<double>(Clock::now() - begin).count();
    if (native_diagnostics) {
      std::fprintf(
        stderr,
        "Diagnostic native %s/%d: operations=%zu seconds=%.6f\n",
        work.name,
        work.workers,
        sample.latency.size(),
        sample.seconds);
    }
    if (statement)
      mysql_stmt_close(statement);
  }
  mysql_thread_end();
  return sample;
}

static void publish(
  const Workload &work,
  const char *driver,
  int repetition,
  std::vector<Samples> samples,
  double cpu,
  unsigned long long cycles)
{
#if !defined(_WIN32)
  const auto phase_end = cpu_phases ? diagnostic_nanoseconds() : 0;
#endif
  double seconds = 0;
  std::vector<double> latency;
  for (auto &sample : samples) {
    seconds = (std::max)(seconds, sample.seconds);
    latency.insert(latency.end(), sample.latency.begin(), sample.latency.end());
  }
  require(!latency.empty() && seconds > 0, "nonempty measurement");
  const auto count = latency.size();
  std::sort(latency.begin(), latency.end());
  const auto p99 = latency[(count - 1) * 99 / 100];
  std::printf(
    "%s,%s,%d,%d,%zu,%.6f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
    work.name,
    driver,
    work.workers,
    repetition,
    count,
    seconds,
    count / seconds,
    p99,
    cpu / seconds * 100,
    static_cast<double>(cycles) / count,
    latency.back());
  std::fflush(stdout);
#if !defined(_WIN32)
  if (cpu_phases) {
    std::fprintf(
      stderr,
      "CPU phase %s/%s/%d/%d: clock=monotonic begin_ns=%llu end_ns=%llu operations=%zu\n",
      work.name,
      driver,
      work.workers,
      repetition,
      cpu_phase_begin,
      phase_end,
      count);
  }
#endif
}

#if defined(WEAVE_PROFILE_RUNTIME)
static weave::Task<weave::Context::Metrics> context_metrics(weave::Context &ctx)
{
  co_return ctx.metrics();
}

static weave::Context::Metrics runtime_metrics(weave::Runtime &runtime, int workers)
{
  weave::Context::Metrics total;
  const std::array fields{
    &weave::Context::Metrics::submitted,
    &weave::Context::Metrics::completed,
    &weave::Context::Metrics::read_calls,
    &weave::Context::Metrics::write_calls,
    &weave::Context::Metrics::dequeue_calls};
  for (int index = 0; index < workers; ++index) {
    auto job = runtime.spawn_on(static_cast<std::size_t>(index), context_metrics);
    require(bool(job), "diagnostic metrics submission");
    auto snapshot = std::move(*job).get();
    require(bool(snapshot), "diagnostic metrics collection");
    for (auto field : fields)
      total.*field += (*snapshot).*field;
  }
  return total;
}
#endif

int main(int argc, char **argv)
{
  if (argc != 4)
    return 2;
  auto port = weave::parse_port(argv[1]);
  auto tls = weave::TlsContext::client({.ca_file = argv[2]});
  if (!port || !tls || mysql_library_init(0, nullptr, nullptr) != 0)
    return 2;
  weave::mysql::Options options{
    .port = *port,
    .user = "weave",
    .password = argv[3],
    .database = "weave_test",
    .tls = *tls,
    .multi_statements = true};
  std::string many = "SELECT 42 AS n";
  for (int index = 1; index < 64; ++index)
    many += " UNION ALL SELECT 42";
  const std::array workloads{
    Workload{"text", "SELECT 42 AS n", 1},
    Workload{"rows64", many, 64},
    Workload{"prepared", "SELECT CAST(? AS SIGNED) AS n", 1, 0, false, true},
    Workload{"blob64k", "SELECT REPEAT('x',65536) AS n", 1, 65536},
    Workload{"stream64", many, 64, 0, true},
    Workload{"multi", "SELECT 42 AS n; SELECT 42 AS n; SELECT 42 AS n; SELECT 42 AS n", 1},
    Workload{"batch4", "SELECT 42 AS n", 1, 0, false, false, true},
    Workload{"reset", "SELECT 42 AS n", 1, 0, false, false, false, true},
    Workload{"connect", "SELECT 42 AS n", 1, 0, false, false, false, false, true},
    Workload{"runtime4", "SELECT 42 AS n", 1, 0, false, false, false, false, false, 4},
    Workload{"runtime4_affine", "SELECT 42 AS n", 1, 0, false, false, false, false, false, 4, true},
#if defined(_WIN32)
    Workload{"runtime4_shared", "SELECT 42 AS n", 1, 0, false, false, false, false, false, 4, false, true},
#endif
  };
  if (const auto *mode = std::getenv("WEAVE_MYSQL_DIAGNOSTICS")) {
#if defined(WEAVE_PROFILE_RUNTIME)
    affine_diagnostics = std::string_view{mode} == "affine";
    native_diagnostics = std::string_view{mode} == "native";
    require(
      affine_diagnostics || native_diagnostics || std::string_view{mode} == "steal",
      "diagnostic mode: affine, steal or native");
    diagnostics = true;
    std::fprintf(stderr, "Instrumented diagnostics only; not performance evidence; mode=%s\n", mode);
#else
    require(false, "diagnostics require a separate WEAVE_PROFILE_RUNTIME build");
#endif
  }
  if (const auto *mode = std::getenv("WEAVE_MYSQL_CPU_PHASES")) {
#if defined(_WIN32)
    require(false, "CPU phase diagnostics require Linux");
#else
    require(std::string_view{mode} == "1", "CPU phase diagnostics: 1");
    require(!diagnostics, "CPU phase diagnostics require an uninstrumented benchmark");
    cpu_phases = true;
#endif
  }
  const auto *diagnostic_workload = diagnostics ? std::getenv("WEAVE_MYSQL_DIAGNOSTIC_WORKLOAD") : nullptr;
  const auto *selected_workload = std::getenv("WEAVE_MYSQL_BENCHMARK_WORKLOAD");
  const auto *selected_driver = std::getenv("WEAVE_MYSQL_BENCHMARK_DRIVER");
  if (selected_driver)
    require(
      std::string_view{selected_driver} == "weave" || std::string_view{selected_driver} == "native",
      "benchmark driver: weave or native");
  if (selected_workload)
    require(
      std::ranges::any_of(
        workloads,
        [&](const auto &work) {
          return work.name == std::string_view{selected_workload};
        }),
      "unknown benchmark workload");
  unsigned repetitions = 7;
  if (const auto *text = std::getenv("WEAVE_MYSQL_BENCHMARK_REPETITIONS")) {
    const std::string_view input{text};
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), repetitions);
    require(
      parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && repetitions > 0 && repetitions <= 31,
      "benchmark repetitions: 1-31");
  }
  auto diagnostic_duration = 100ms;
  if (diagnostics) {
    if (const auto *text = std::getenv("WEAVE_MYSQL_DIAGNOSTIC_MS")) {
      const std::string_view input{text};
      unsigned value = 0;
      const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
      require(
        parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && value > 0 && value <= 10000,
        "diagnostic duration: 1-10000 ms");
      diagnostic_duration = std::chrono::milliseconds{value};
    }
  }
  if (diagnostic_workload) {
    require(
      std::ranges::any_of(
        workloads,
        [&](const auto &work) {
          return work.name == std::string_view{diagnostic_workload};
        }),
      "unknown diagnostic workload");
  }
  if (!diagnostics) {
#if defined(WEAVE_PROFILE_RUNTIME) || defined(WEAVE_TRACE_RUNTIME)
    require(false, "instrumented builds cannot produce timing evidence");
#endif
    std::fprintf(
      stderr,
      "Native MySQL client: %s; verified TLS; policy=TLSv1.3 TLS_AES_256_GCM_SHA384; "
      "warmup 150ms; %u x 750ms measurements\n",
      mysql_get_client_info(),
      repetitions);
#if !defined(_WIN32)
    if (cpu_phases)
      std::fprintf(stderr, "CPU phase diagnostics only; not performance evidence; clock=monotonic; units=ns\n");
#endif
    std::puts(
      "workload,driver,workers,repetition,operations,seconds,ops_per_second,p99_us,cpu_percent,cycles_per_operation,"
      "max_us");
  }
  for (const auto &work : workloads) {
    if (selected_workload && work.name != std::string_view{selected_workload})
      continue;
    if (diagnostic_workload && work.name != std::string_view{diagnostic_workload})
      continue;
    for (int repetition = diagnostics ? 1 : 0; repetition <= (diagnostics ? 1 : static_cast<int>(repetitions));
      ++repetition) {
      const auto duration = diagnostics ? diagnostic_duration : repetition == 0 ? 150ms : 750ms;
      // Alternate paired driver order; all measurements are sequential, never competing processes.
      for (int order = 0; order < 2; ++order) {
        const bool native = (order + repetition) % 2 == 0;
        if (selected_driver && native != (std::string_view{selected_driver} == "native"))
          continue;
        if (diagnostics && native != native_diagnostics)
          continue;
        std::vector<Samples> samples(work.workers);
        if (native) {
          std::barrier start(work.workers + 1);
          std::atomic<int> ready{0};
          std::vector<std::thread> threads;
          for (int index = 0; index < work.workers; ++index) {
            threads.emplace_back([&, index] {
              samples[index] = native_worker(*port, argv[2], argv[3], work, duration, ready, start);
            });
          }
          // Native connections and statement preparation complete before the barrier releases timing.
          while (ready.load(std::memory_order_acquire) != work.workers)
            std::this_thread::sleep_for(1ms);
          const auto cpu = cpu_seconds();
          const auto cycles = cpu_cycles();
#if !defined(_WIN32)
          if (cpu_phases && repetition)
            cpu_phase_begin = diagnostic_nanoseconds();
#endif
          start.arrive_and_wait();
          for (auto &thread : threads)
            thread.join();
          if (repetition && !diagnostics)
            publish(work, "native", repetition, std::move(samples), cpu_seconds() - cpu, cpu_cycles() - cycles);
        } else {
          auto runtime = weave::Runtime::create(
            {.workers = static_cast<std::size_t>(work.workers),
              .scheduler = affine_diagnostics || work.affine ? weave::Scheduler::worker_affine
                                                             : weave::Scheduler::work_stealing,
              .io_layout = work.shared ? weave::IoLayout::shared : weave::IoLayout::sharded});
          require(bool(runtime), "Runtime startup");
          std::atomic<int> ready{0};
          weave::Channel<int> start(work.workers);
          std::vector<weave::JoinHandle<Samples>> jobs;
          for (int index = 0; index < work.workers; ++index) {
            auto job = runtime->spawn(weave_worker(options, work, duration, ready, start));
            require(bool(job), "Runtime submission");
            jobs.push_back(std::move(*job));
          }
          while (ready.load(std::memory_order_acquire) != work.workers) {
            for (auto &job : jobs) {
              if (job.ready()) {
                auto failed = std::move(job).get();
                require(false, failed ? "worker completed before start" : failed.error().message().c_str());
              }
            }
            std::this_thread::sleep_for(1ms);
          }
          const auto cpu = cpu_seconds();
          const auto cycles = cpu_cycles();
#if defined(WEAVE_PROFILE_RUNTIME)
          const auto io_before = diagnostics ? runtime_metrics(*runtime, work.workers) : weave::Context::Metrics{};
#endif
#if !defined(_WIN32)
          if (cpu_phases && repetition)
            cpu_phase_begin = diagnostic_nanoseconds();
#endif
          for (int index = 0; index < work.workers; ++index) {
            int value = 1;
            require(bool(start.try_send(value)), "benchmark start");
          }
          for (int index = 0; index < work.workers; ++index) {
            auto result = std::move(jobs[index]).get();
            require(bool(result), result ? "" : result.error().message().c_str());
            samples[index] = std::move(*result);
          }
#if defined(WEAVE_PROFILE_RUNTIME)
          if (diagnostics) {
            const auto io = runtime_metrics(*runtime, work.workers);
            std::size_t count = 0;
            for (const auto &sample : samples)
              count += sample.latency.size();
            const auto operations = static_cast<double>(count);
            std::fprintf(
              stderr,
              "Diagnostic IO %s/%d: operations=%zu submitted/op=%.3f completed/op=%.3f "
              "reads/op=%.3f writes/op=%.3f dequeue/op=%.3f\n",
              work.name,
              work.workers,
              count,
              (io.submitted - io_before.submitted) / operations,
              (io.completed - io_before.completed) / operations,
              (io.read_calls - io_before.read_calls) / operations,
              (io.write_calls - io_before.write_calls) / operations,
              (io.dequeue_calls - io_before.dequeue_calls) / operations);
          }
#endif
          if (repetition && !diagnostics)
            publish(work, "weave", repetition, std::move(samples), cpu_seconds() - cpu, cpu_cycles() - cycles);
        }
      }
    }
  }
  mysql_library_end();
}
