#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <libpq-fe.h>
#include <libpq/libpq-fs.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

using Clock = std::chrono::steady_clock;

struct Configuration {
  std::string library;
  std::string workload;
  weave::pg::Options database;
  std::string ca;
  std::size_t count = 1000;
  std::size_t batch = 32;
};

struct Metrics {
  std::vector<double> latency;
  weave::u64 checksum = 14695981039346656037ULL;
  std::size_t operations = 0;
  double seconds = 0;
  double cpu = 0;
};

static double process_cpu()
{
#ifdef _WIN32
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

static void consume(Metrics &metrics, std::string_view value, bool null)
{
  metrics.checksum ^= static_cast<weave::u64>(null);
  metrics.checksum *= 1099511628211ULL;
  for (auto byte : value) {
    metrics.checksum ^= static_cast<unsigned char>(byte);
    metrics.checksum *= 1099511628211ULL;
  }
}

static void consume(Metrics &metrics, const weave::pg::ResultSet &result)
{
  for (const auto &row : result.rows) {
    for (const auto &value : row)
      consume(metrics, value.bytes(), value.is_null());
  }
}

static bool consume(Metrics &metrics, PGresult *result)
{
  if (!result)
    return false;

  auto status = PQresultStatus(result);
  bool valid = status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK;
  if (valid) {
    auto rows = PQntuples(result);
    auto columns = PQnfields(result);
    for (int row = 0; row < rows; ++row) {
      for (int column = 0; column < columns; ++column) {
        auto value = PQgetvalue(result, row, column);
        auto size = static_cast<std::size_t>(PQgetlength(result, row, column));
        consume(metrics, std::string_view{value, size}, PQgetisnull(result, row, column) != 0);
      }
    }
  } else {
    std::fprintf(stderr, "%s", PQresultErrorMessage(result));
  }

  PQclear(result);
  return valid;
}

static std::string sql(const Configuration &configuration)
{
  if (configuration.workload == "rows")
    return "SELECT i, repeat('x', 256) FROM generate_series(1, 1000) i";
  if (configuration.workload == "extended" || configuration.workload == "binary" ||
    configuration.workload == "prepared" || configuration.workload == "batch")
    return "SELECT $1::int + 1";

  return "SELECT 42::int";
}

static void record(Metrics &metrics, Clock::time_point start, std::size_t operations)
{
  auto elapsed = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
  metrics.latency.push_back(elapsed / static_cast<double>(operations));
  metrics.operations += operations;
}

static std::string large_object_payload()
{
  std::string bytes(65536, '\0');
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = static_cast<char>(index & 255);

  return bytes;
}

static weave::Task<void> weave_large_objects(
  weave::pg::Connection &connection,
  const Configuration &configuration,
  Metrics &metrics)
{
  namespace lo = weave::pg::lo;
  auto payload = large_object_payload();
  std::string received(payload.size(), '\0');
  auto input = std::as_bytes(std::span{payload.data(), payload.size()});
  auto output = std::as_writable_bytes(std::span{received.data(), received.size()});
  bool reading = configuration.workload == "lo_read";
  bool appending = configuration.workload == "lo_append";
  co_await connection.query("BEGIN");
  auto object = co_await lo::create(connection);
  auto descriptor = co_await lo::open(connection, object, lo::Access::read_write);
  co_await lo::write_all(connection, descriptor, input);

  auto start = Clock::now();
  auto cpu = process_cpu();
  for (std::size_t index = 0; index < configuration.count; ++index) {
    auto sample = Clock::now();
    auto offset = appending ? static_cast<weave::i64>(index * payload.size()) : 0;
    if (co_await lo::seek(connection, descriptor, offset) != offset)
      co_await weave::fail(std::errc::bad_message);
    if (reading) {
      if (co_await lo::read(connection, descriptor, output) != output.size() || received != payload)
        co_await weave::fail(std::errc::bad_message);
      consume(metrics, received, false);
    } else {
      co_await lo::write_all(connection, descriptor, input);
      consume(metrics, std::to_string(input.size()), false);
    }

    record(metrics, sample, 1);
  }

  metrics.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  metrics.cpu = process_cpu() - cpu;
  auto final_offset = appending ? static_cast<weave::i64>((configuration.count - 1) * payload.size()) : 0;
  co_await lo::seek(connection, descriptor, final_offset);
  if (co_await lo::read(connection, descriptor, output) != output.size() || received != payload)
    co_await weave::fail(std::errc::bad_message);

  co_await lo::close(connection, descriptor);
  co_await lo::remove(connection, object);
  co_await connection.query("ROLLBACK");
}

static weave::Task<void> weave_loop(const Configuration &configuration, Metrics &metrics)
{
  if (configuration.workload == "connect") {
    auto start = Clock::now();
    auto cpu = process_cpu();
    for (std::size_t index = 0; index < configuration.count; ++index) {
      auto sample = Clock::now();
      auto connection = co_await weave::pg::connect(configuration.database);
      co_await connection.finish();
      record(metrics, sample, 1);
    }

    metrics.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    metrics.cpu = process_cpu() - cpu;
    co_return;
  }

  auto connection = co_await weave::pg::connect(configuration.database);
  if (configuration.workload == "lo_read" || configuration.workload == "lo_write" ||
    configuration.workload == "lo_append") {
    co_await weave_large_objects(connection, configuration, metrics);
    co_await connection.finish();
    co_return;
  }
  auto statement = sql(configuration);
  std::vector<weave::pg::Parameter> parameters{{"41", 23}};
  bool prepared = configuration.workload == "prepared";
  bool binary = configuration.workload == "binary";
  auto format = binary ? weave::pg::Format::binary : weave::pg::Format::text;
  if (prepared) {
    std::vector<weave::u32> types{23};
    co_await connection.prepare("bench", statement, types);
  }

  std::vector<weave::pg::Command> commands(configuration.batch, {statement, parameters});
  if (configuration.workload == "copy")
    co_await connection.query("CREATE TEMP TABLE weave_bench (value int, text text)");

  std::string copy;
  for (std::size_t row = 0; row < 1000; ++row)
    copy += std::to_string(row) + "\tpayload\n";

  auto start = Clock::now();
  auto cpu = process_cpu();
  for (std::size_t index = 0; index < configuration.count; ++index) {
    auto sample = Clock::now();
    std::size_t operations = 1;
    if (configuration.workload == "simple" || configuration.workload == "rows") {
      auto results = co_await connection.query(statement);
      for (const auto &result : results)
        consume(metrics, result);
    } else if (configuration.workload == "batch") {
      auto outcomes = co_await connection.batch(commands);
      for (const auto &outcome : outcomes) {
        if (!outcome.result || outcome.aborted || !outcome.error.fields.empty())
          co_await weave::fail(std::errc::bad_message);

        consume(metrics, *outcome.result);
      }

      operations = commands.size();
    } else if (configuration.workload == "copy") {
      co_await connection.start_copy("COPY weave_bench FROM STDIN");
      co_await connection.write_copy(std::as_bytes(std::span{copy.data(), copy.size()}));
      auto result = co_await connection.end_copy();
      if (result.command != "COPY 1000")
        co_await weave::fail(std::errc::bad_message);

      consume(metrics, result.command, false);
    } else {
      auto result = prepared ? co_await connection.execute_prepared("bench", parameters, format)
                             : co_await connection.execute(statement, parameters, format);
      consume(metrics, result);
    }

    record(metrics, sample, operations);
  }

  metrics.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  metrics.cpu = process_cpu() - cpu;
  co_await connection.finish();
}

static PGconn *pq_connect(const Configuration &configuration)
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
    configuration.database.user.c_str(),
    configuration.database.database.c_str(),
    configuration.database.password.c_str(),
    "weave-bench",
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
    "30",
    nullptr};
  return PQconnectdbParams(keys, values, 0);
}

static bool pq_large_objects(PGconn *connection, const Configuration &configuration, Metrics &metrics)
{
  auto payload = large_object_payload();
  std::string received(payload.size(), '\0');
  bool reading = configuration.workload == "lo_read";
  bool appending = configuration.workload == "lo_append";
  if (!consume(metrics, PQexec(connection, "BEGIN")))
    return false;
  auto object = lo_create(connection, 0);
  if (object == 0)
    return false;
  auto descriptor = lo_open(connection, object, INV_READ | INV_WRITE);
  if (descriptor < 0 ||
    lo_write(connection, descriptor, payload.data(), payload.size()) != static_cast<int>(payload.size()))
    return false;

  auto start = Clock::now();
  auto cpu = process_cpu();
  for (std::size_t index = 0; index < configuration.count; ++index) {
    auto sample = Clock::now();
    auto offset = appending ? static_cast<pg_int64>(index * payload.size()) : 0;
    if (lo_lseek64(connection, descriptor, offset, SEEK_SET) != offset)
      return false;
    if (reading) {
      if (lo_read(connection, descriptor, received.data(), received.size()) != static_cast<int>(received.size()) ||
        received != payload)
        return false;
      consume(metrics, received, false);
    } else {
      auto size = lo_write(connection, descriptor, payload.data(), payload.size());
      if (size != static_cast<int>(payload.size()))
        return false;
      consume(metrics, std::to_string(size), false);
    }

    record(metrics, sample, 1);
  }

  metrics.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  metrics.cpu = process_cpu() - cpu;
  auto final_offset = appending ? static_cast<pg_int64>((configuration.count - 1) * payload.size()) : 0;
  if (lo_lseek64(connection, descriptor, final_offset, SEEK_SET) != final_offset ||
    lo_read(connection, descriptor, received.data(), received.size()) != static_cast<int>(received.size()) ||
    received != payload)
    return false;

  if (lo_close(connection, descriptor) != 0 || lo_unlink(connection, object) != 1)
    return false;

  return consume(metrics, PQexec(connection, "ROLLBACK"));
}

static bool pq_loop(const Configuration &configuration, Metrics &metrics)
{
  if (configuration.workload == "connect") {
    auto start = Clock::now();
    auto cpu = process_cpu();
    for (std::size_t index = 0; index < configuration.count; ++index) {
      auto sample = Clock::now();
      auto connection = pq_connect(configuration);
      bool ready = PQstatus(connection) == CONNECTION_OK;
      if (!ready)
        std::fprintf(stderr, "%s", PQerrorMessage(connection));

      PQfinish(connection);
      if (!ready)
        return false;

      record(metrics, sample, 1);
    }

    metrics.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    metrics.cpu = process_cpu() - cpu;
    return true;
  }

  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{pq_connect(configuration), PQfinish};
  if (PQstatus(connection.get()) != CONNECTION_OK) {
    std::fprintf(stderr, "%s", PQerrorMessage(connection.get()));
    return false;
  }

  if (configuration.workload == "lo_read" || configuration.workload == "lo_write" ||
    configuration.workload == "lo_append") {
    bool completed = pq_large_objects(connection.get(), configuration, metrics);
    if (!completed)
      std::fprintf(stderr, "Large-object comparison failed: %s", PQerrorMessage(connection.get()));
    return completed;
  }

  auto statement = sql(configuration);
  const char *parameters[] = {"41"};
  const Oid types[] = {23};
  bool prepared = configuration.workload == "prepared";
  int binary = configuration.workload == "binary" ? 1 : 0;
  if (prepared && !consume(metrics, PQprepare(connection.get(), "bench", statement.c_str(), 1, types)))
    return false;
  if (configuration.workload == "batch" && !PQenterPipelineMode(connection.get()))
    return false;
  if (configuration.workload == "copy" &&
    !consume(metrics, PQexec(connection.get(), "CREATE TEMP TABLE weave_bench (value int, text text)")))
    return false;

  std::string copy;
  for (std::size_t row = 0; row < 1000; ++row)
    copy += std::to_string(row) + "\tpayload\n";

  auto start = Clock::now();
  auto cpu = process_cpu();
  for (std::size_t index = 0; index < configuration.count; ++index) {
    auto sample = Clock::now();
    std::size_t operations = 1;
    if (configuration.workload == "simple" || configuration.workload == "rows") {
      if (!consume(metrics, PQexec(connection.get(), statement.c_str())))
        return false;
    } else if (configuration.workload == "batch") {
      for (std::size_t query = 0; query < configuration.batch; ++query) {
        if (!PQsendQueryParams(connection.get(), statement.c_str(), 1, types, parameters, nullptr, nullptr, 0))
          return false;
      }

      if (!PQpipelineSync(connection.get()))
        return false;

      std::size_t complete = 0;
      bool sync = false;
      while (!sync) {
        auto result = PQgetResult(connection.get());
        if (!result && PQstatus(connection.get()) != CONNECTION_OK) {
          std::fprintf(stderr, "%s", PQerrorMessage(connection.get()));
          return false;
        }
        if (!result)
          continue;

        if (PQresultStatus(result) == PGRES_PIPELINE_SYNC) {
          PQclear(result);
          sync = true;
        } else {
          if (!consume(metrics, result))
            return false;

          ++complete;
        }
      }

      if (complete != configuration.batch || PQgetResult(connection.get()) != nullptr)
        return false;

      operations = configuration.batch;
    } else if (configuration.workload == "copy") {
      auto result = PQexec(connection.get(), "COPY weave_bench FROM STDIN");
      bool copying = result && PQresultStatus(result) == PGRES_COPY_IN;
      PQclear(result);
      if (!copying || PQputCopyData(connection.get(), copy.data(), static_cast<int>(copy.size())) != 1 ||
        PQputCopyEnd(connection.get(), nullptr) != 1)
        return false;

      result = PQgetResult(connection.get());
      if (!result || PQresultStatus(result) != PGRES_COMMAND_OK || std::strcmp(PQcmdStatus(result), "COPY 1000") != 0) {
        PQclear(result);
        return false;
      }

      consume(metrics, PQcmdStatus(result), false);
      PQclear(result);
      if (PQgetResult(connection.get()) != nullptr)
        return false;
    } else {
      auto result = prepared
        ? PQexecPrepared(connection.get(), "bench", 1, parameters, nullptr, nullptr, binary)
        : PQexecParams(connection.get(), statement.c_str(), 1, types, parameters, nullptr, nullptr, binary);
      if (!consume(metrics, result))
        return false;
    }

    record(metrics, sample, operations);
  }

  metrics.seconds = std::chrono::duration<double>(Clock::now() - start).count();
  metrics.cpu = process_cpu() - cpu;
  if (configuration.workload == "batch" && !PQexitPipelineMode(connection.get()))
    return false;

  return true;
}

static weave::Task<std::size_t> weave_memory(
  const Configuration &configuration,
  Metrics &metrics,
  const std::string &query)
{
  auto connection = co_await weave::pg::connect(configuration.database);
  auto results = co_await connection.query(query);
  if (results.size() != 1)
    co_await weave::fail(weave::pg::make_error_code(weave::pg::Error::protocol));

  consume(metrics, results.front());
  metrics.operations = results.front().rows.size();
  const auto retained = results.front().memory_size();
  co_await connection.finish();
  co_return retained;
}

static int result_memory(const Configuration &configuration)
{
  const auto query = "SELECT i, repeat('x', " + std::to_string(configuration.batch) + ") FROM generate_series(1, " +
    std::to_string(configuration.count) + ") AS i";
  Metrics metrics;
  std::size_t retained = 0;
  if (configuration.library == "weave") {
    auto ctx = weave::Context::create();
    if (!ctx)
      return weave::report_error(ctx.error());
    auto result = ctx->run(weave_memory(configuration, metrics, query));
    if (!result)
      return weave::report_error(result.error());
    retained = *result;
  } else {
    std::unique_ptr<PGconn, decltype(&PQfinish)> connection{pq_connect(configuration), PQfinish};
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK)
      return EXIT_FAILURE;
    auto *result = PQexec(connection.get(), query.c_str());
    if (!result)
      return EXIT_FAILURE;
    retained = PQresultMemorySize(result);
    metrics.operations = static_cast<std::size_t>(PQntuples(result));
    if (!consume(metrics, result))
      return EXIT_FAILURE;
  }
  if (metrics.operations != configuration.count)
    return EXIT_FAILURE;
  std::printf(
    "{\"rows\":%zu,\"payload_width\":%zu,\"retained_bytes\":%zu,\"checksum\":%llu,\"libpq_version\":%d}\n",
    metrics.operations,
    configuration.batch,
    retained,
    static_cast<unsigned long long>(metrics.checksum),
    PQlibVersion());
  return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
  if (argc != 8) {
    std::fprintf(stderr, "Usage: compare weave|libpq workload iterations batch host port ca-file-or-plain\n");
    return 2;
  }

  Configuration configuration;
  configuration.library = argv[1];
  configuration.workload = argv[2];
  auto count = weave::pg::Value{{argv[3]}}.integer<std::size_t>();
  auto batch = weave::pg::Value{{argv[4]}}.integer<std::size_t>();
  auto port = weave::parse_port(argv[6]);
  const std::vector<std::string_view> workloads{
    "connect",
    "simple",
    "extended",
    "prepared",
    "binary",
    "batch",
    "rows",
    "copy",
    "lo_read",
    "lo_write",
    "lo_append",
    "memory"};
  bool known = std::find(workloads.begin(), workloads.end(), configuration.workload) != workloads.end();
  if (!count || !*count || !batch || !*batch || *batch > 65535 || !port || !known ||
    (configuration.library != "weave" && configuration.library != "libpq"))
    return 2;
  if (configuration.workload == "lo_append" && *count > 8192)
    return 2;
  if (configuration.workload == "memory" && (*count > 10000 || *batch > 1024))
    return 2;

  configuration.count = *count;
  configuration.batch = *batch;
  configuration.database.host = argv[5];
  configuration.database.port = *port;
  configuration.database.user = "weave";
  configuration.database.database = "postgres";
  configuration.database.application_name = "weave-bench";
  configuration.database.min_protocol = weave::pg::ProtocolVersion::v30;
  configuration.database.max_protocol = weave::pg::ProtocolVersion::v30;
  configuration.database.authentication.methods = {weave::pg::Authentication::scram_sha256};
  configuration.database.client_certificate = weave::TlsCertificateMode::disable;
  auto password = std::getenv("WEAVE_PG_PASSWORD");
  if (!password)
    return 2;

  configuration.database.password = password;
  configuration.database.plaintext = std::string_view{argv[7]} == "plain";
  if (!configuration.database.plaintext) {
    configuration.ca = argv[7];
    auto credentials = weave::TlsContext::client(
      {.ca_file = configuration.ca, .min_version = weave::TlsVersion::tls13, .max_version = weave::TlsVersion::tls13});
    if (!credentials)
      return weave::report_error(credentials.error());

    configuration.database.tls = *credentials;
    configuration.database.channel_binding = weave::pg::ChannelBinding::require;
  }

  if (configuration.workload == "memory")
    return result_memory(configuration);

  Metrics metrics;
  metrics.latency.reserve(configuration.count);
  if (configuration.library == "weave") {
    auto ctx = weave::Context::create();
    if (!ctx)
      return weave::report_error(ctx.error());

    auto result = ctx->run(weave_loop(configuration, metrics));
    if (!result)
      return weave::report_error(result.error());
  } else if (!pq_loop(configuration, metrics)) {
    return 1;
  }

  std::sort(metrics.latency.begin(), metrics.latency.end());
  auto percentile = [&](double fraction) {
    return metrics.latency[static_cast<std::size_t>(fraction * static_cast<double>(metrics.latency.size() - 1))];
  };
  std::printf(
    "{\"operations\":%zu,\"seconds\":%.9f,\"cpu_seconds\":%.9f,\"ops_per_second\":%.3f,"
    "\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,\"checksum\":%llu,\"libpq_version\":%d}\n",
    metrics.operations,
    metrics.seconds,
    metrics.cpu,
    static_cast<double>(metrics.operations) / metrics.seconds,
    percentile(0.50),
    percentile(0.95),
    percentile(0.99),
    static_cast<unsigned long long>(metrics.checksum),
    PQlibVersion());
}
