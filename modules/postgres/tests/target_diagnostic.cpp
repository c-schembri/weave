#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#if defined(WEAVE_POSTGRES_TEST_LIBPQ_VERSION)
#include <libpq-fe.h>
#endif
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace pg = weave::pg;
static std::atomic<unsigned> checks = 0;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Target diagnostic check failed at line %u\n", location.line());
    std::exit(EXIT_FAILURE);
  }
}

static void failure(std::error_code error, const pg::Diagnostic &diagnostic, std::string_view mode)
{
  if (mode == "sql")
    check(pg::sqlstate(error) == "42501");
  else if (mode == "cancel" || mode == "cancel_after_sql")
    check(error == std::errc::operation_canceled);
  else if (mode == "eof")
    check(error == std::errc::connection_reset);
  else if (mode == "reject")
    check(error == pg::Error::target_session);
  else
    check(error == pg::Error::protocol);

  const bool received_error = mode == "sql" || mode == "protocol_after_sql" || mode == "cancel_after_sql";
  if (received_error) {
    check(diagnostic.sqlstate() == "42501");
    check(diagnostic.message() == "target policy denied");
    check(diagnostic.field('D') == "independent detail");
    check(diagnostic.field('H') == "independent hint");
    auto retained = diagnostic;
    check(retained.fields == diagnostic.fields);
    auto formatted = retained.format();
    check(formatted && *formatted == "ERROR:  target policy denied\n");
  } else {
    check(diagnostic.fields.empty());
  }
}

static weave::Task<void> exercise(weave::Context &ctx, pg::Options options, std::string mode)
{
  pg::Diagnostic diagnostic{{{'M', "stale caller text"}}};
  weave::CancelSource cancellation;
  auto operation = pg::connect(options, diagnostic, [&](const pg::Diagnostic &notice) noexcept {
    if (notice.message() == "PENDING")
      cancellation.cancel();
  });
  auto job = ctx.spawn(std::move(operation), {.cancel = cancellation.token()});
  check(job.has_value());
  auto connection = co_await weave::as_result(std::move(*job));
  if (mode == "success" || mode == "any") {
    check(connection.has_value());
    check(diagnostic.fields.empty());
    co_await connection->finish();
  } else {
    check(!connection);
    failure(connection.error(), diagnostic, mode);
  }
}

int main(int argc, char **argv)
{
  check(argc == 4);
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  std::string mode = argv[2];
  std::string_view engine = argv[3];

  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "probe", .plaintext = true};
  options.target_session = mode == "any" ? pg::TargetSession::any : pg::TargetSession::read_only;
  options.hosts = {{"127.0.0.1", *port}, {"127.0.0.1", *port}};

  if (engine == "native") {
#if defined(WEAVE_POSTGRES_TEST_LIBPQ_VERSION)
    check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
    const std::array<const char *, 8>
      keys{"host", "port", "user", "dbname", "sslmode", "gssencmode", "target_session_attrs", nullptr};
    auto ports = std::to_string(*port) + "," + std::to_string(*port);
    const std::array<const char *, 8> values{
      "127.0.0.1,127.0.0.1",
      ports.c_str(),
      "probe",
      "probe",
      "disable",
      "disable",
      mode == "any" ? "any" : "read-only",
      nullptr};
    auto connection = PQconnectdbParams(keys.data(), values.data(), 0);
    check(connection != nullptr);
    if (mode == "success" || mode == "any")
      check(PQstatus(connection) == CONNECTION_OK);
    else {
      check(PQstatus(connection) == CONNECTION_BAD);
      if (mode == "sql")
        check(std::string_view{PQerrorMessage(connection)}.find("target policy denied") != std::string_view::npos);
    }
    PQfinish(connection);
#else
    check(false);
#endif
  } else if (engine == "blocking") {
    pg::Diagnostic diagnostic{{{'M', "stale caller text"}}};
    auto connection = pg::BlockingConnection::connect(options, diagnostic);
    if (mode == "success" || mode == "any") {
      check(connection.has_value());
      check(diagnostic.fields.empty());
      check(connection->finish().has_value());
    } else {
      check(!connection);
      failure(connection.error(), diagnostic, mode);
    }
  } else if (engine == "context") {
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    check(ctx->run(exercise(*ctx, options, mode)).has_value());
    check(ctx->metrics().submitted == ctx->metrics().completed);
  } else {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    auto runtime = weave::Runtime::create(
      {.workers = 4,
        .scheduler = engine == "affine" ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing});
    check(runtime.has_value());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn([options, mode](weave::Context &ctx) {
        return exercise(ctx, options, mode);
      });
      check(job.has_value());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      check(std::move(job).get().has_value());
#else
    check(false);
#endif
  }
  std::printf("Target diagnostic controls passed: %u checks\n", checks.load());
}
