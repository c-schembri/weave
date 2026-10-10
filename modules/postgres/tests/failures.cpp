#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/channel.hpp>
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <algorithm>
#include <openssl/crypto.h>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks = 0;

static void check(bool condition, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Failure session check failed at line %u\n", location.line());
    std::exit(1);
  }
}

template <class C>
static pg::Failure failure_snapshot(C &connection, std::error_code error)
{
  auto transaction = connection.transaction();
  if (connection.open())
    check(transaction != pg::Transaction::unknown && transaction != pg::Transaction::in_progress);
  else
    check(transaction == pg::Transaction::unknown);

  auto snapshot = connection.last_failure();
  check(snapshot.has_value());
  check(snapshot->error == error);
  check(snapshot->diagnostic.fields == connection.last_error().fields);
  auto text = snapshot->format();
  check(text.has_value());
  auto independent = *snapshot;
  snapshot->diagnostic.fields.clear();
  check(independent.diagnostic.fields == connection.last_error().fields);
  auto configuration = connection.configuration();
  check(configuration.has_value());
  check(configuration->host == "127.0.0.1" && configuration->port != 0);
  check(configuration->user == "test" && configuration->database == "test");
  check(configuration->application_name == "weave" && configuration->client_encoding == "UTF8");
  check(configuration->plaintext && !configuration->tls_context && !configuration->tls_options);
  check(configuration->hosts.empty() && !configuration->oauth && !configuration->gss_context);
  check(configuration->limits.message_bytes == 4096 || configuration->limits.message_bytes == 16 * 1024 * 1024);
  check(configuration->limits.result_bytes == configuration->limits.message_bytes);
  configuration->user = "mutated-copy";
  check(connection.configuration()->user == "test");
  check(connection.last_failure()->error == error);
  check(connection.last_failure()->diagnostic.fields == independent.diagnostic.fields);
  return independent;
}

static void formatting()
{
  pg::Failure empty;
  check(empty.format() && empty.format()->empty());
  pg::Failure failure{std::make_error_code(std::errc::connection_reset), {}};
  auto minimal = failure.format();
  check(minimal.has_value());
  check(failure.format({.output_bytes = minimal->size()}).has_value());
  auto short_output = failure.format({.output_bytes = minimal->size() - 1});
  check(!short_output && short_output.error() == pg::Error::resource_limit);
  failure.diagnostic.fields = {{'S', "ERROR"}, {'C', "22012"}, {'M', "retained failure"}, {'D', "hidden"}};
  auto text = failure.format();
  check(text && text->find("retained failure") != std::string::npos);
  check(text->find("hidden") == std::string::npos);
  auto verbose = failure.format({.verbosity = pg::DiagnosticVerbosity::standard});
  check(verbose && verbose->find("hidden") != std::string::npos);
  auto input_limit = failure.format({.input_bytes = 1});
  check(!input_limit && input_limit.error() == pg::Error::resource_limit);
  failure.diagnostic.fields.emplace_back('M', "duplicate");
  check(!failure.format());
  failure.diagnostic.fields.pop_back();
  failure.error = {};
  check(!failure.format());
}

static void outcomes(const std::vector<pg::Outcome> &results)
{
  check(results.size() == 4);
  check(results[0].result && results[0].result->kind == pg::ResultKind::command);
  check(results[1].result && results[1].result->kind == pg::ResultKind::empty_query);
  check(results[2].result && results[2].result->kind == pg::ResultKind::tuples);
  check(results[2].result->columns.empty() && results[2].result->rows.size() == 1);
  check(!results[3].result && !results[3].aborted && results[3].error.sqlstate() == "22012");
  check(results[3].error.message() == "retained failure" && results[3].error.field('H') == "hint");
  check(pg::status_name(results[0]) == "command");
  check(pg::status_name(results[1]) == "empty_query");
  check(pg::status_name(results[2]) == "tuples");
  check(pg::status_name(results[3]) == "sql_error");
  check(pg::status_name(results[3].error) == "diagnostic");
}

static weave::Task<void> phase(pg::Connection &connection, std::string_view mode)
{
  if (mode == "row_read") {
    co_await connection.start_rows("PROBE");
    co_await connection.read_row();
  } else if (mode == "copy_read") {
    co_await connection.start_copy("PROBE");
    co_await connection.read_copy();
  } else if (mode == "copy_write") {
    co_await connection.start_copy("PROBE");
    std::vector<std::byte> data(8 * 1024 * 1024);
    co_await connection.write_copy(data);
    // A send may queue successfully before the peer's reset reaches this socket.
    co_await connection.end_copy();
  } else if (mode == "exchange_read") {
    auto exchange = co_await connection.exchange("PROBE");
    co_await exchange.next();
  } else if (mode == "notification") {
    co_await connection.wait_notification();
  } else if (mode == "function") {
    co_await connection.call_function(1234);
  } else if (mode == "pipeline_duplex" || mode == "pipeline_split") {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    check(pipeline->execute(pg::Command{.sql = "PROBE"}).has_value());
    check(pipeline->sync().has_value());
    if (mode == "pipeline_duplex")
      co_await pipeline->flush();
    else
      co_await weave::when_all(pipeline->send(), pipeline->receive());
  } else if (mode == "batch") {
    co_await connection.batch({pg::Command{.sql = "PROBE"}});
  } else if (mode == "encoding") {
    co_await connection.set_client_encoding(pg::Encoding::latin1);
  }
}

static weave::Task<void> exercise(weave::Context &ctx, pg::Options options, std::string mode)
{
  auto connection = co_await pg::connect(options);
  failure_snapshot(connection, {});
  auto invalid = co_await weave::as_result(connection.query_outcomes(std::string{"bad\0sql", 7}));
  check(!invalid && invalid.error() == std::errc::invalid_argument && connection.open());
  {
    auto deferred = connection.query_outcomes("must_not_send");
    auto blocked_snapshot = connection.last_failure();
    check(!blocked_snapshot && blocked_snapshot.error() == pg::Error::busy);
    auto blocked = co_await weave::as_result(connection.reset(options));
    check(!blocked && blocked.error() == pg::Error::busy && connection.open());
  }
  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    auto blocked = co_await weave::as_result(connection.query_outcomes("must_not_send"));
    check(!blocked && blocked.error() == pg::Error::busy);
    auto rejected_password = co_await weave::as_result(connection.password_verifier("user", "password"));
    check(!rejected_password && rejected_password.error() == pg::Error::busy);
    auto rejected_change = co_await weave::as_result(connection.change_password("user", "password"));
    check(!rejected_change && rejected_change.error() == pg::Error::busy);
    check(pipeline->finish().has_value());
  }
  failure_snapshot(connection, {});
  const std::array phase_modes{
    "row_read",
    "copy_read",
    "copy_write",
    "exchange_read",
    "notification",
    "function",
    "pipeline_duplex",
    "pipeline_split",
    "batch",
    "encoding"};
  if (std::ranges::find(phase_modes, mode) != phase_modes.end()) {
    auto result = co_await weave::as_result(phase(connection, mode));
    check(!result);
    auto snapshot = failure_snapshot(connection, result.error());
    auto moved = std::move(connection);
    auto missing = connection.last_failure();
    check(!missing && missing.error() == pg::Error::closed);
    check(moved.last_failure()->error == snapshot.error);
    check(!moved.open());
    check(snapshot.format().has_value());
    co_return;
  }
  if (mode.starts_with("extended")) {
    auto result = co_await weave::as_result(connection.execute_outcome("PROBE"));
    if (mode == "extended_error") {
      check(result && !result->result && result->error.sqlstate() == "22012" && connection.open());
      check(pg::status_name(*result) == "sql_error");
      failure_snapshot(connection, pg::sql_error("22012"));
      auto retained = *result;
      co_await connection.finish();
      check(retained.error.message() == "retained failure");
    } else {
      check(!result && result.error() == pg::Error::protocol && !connection.open());
      check(pg::status_name(result.error()) == "protocol_error");
      failure_snapshot(connection, result.error());
    }
    co_return;
  }
  if (mode == "cancel") {
    weave::Channel<int> markers{1};
    weave::CancelSource cancellation;
    check(connection
        .on_notice([&](const pg::Diagnostic &notice) noexcept {
          if (notice.message() == "PENDING") {
            int marker = 1;
            check(markers.try_send(marker).has_value());
            cancellation.cancel();
          }
        })
        .has_value());
    auto job = ctx.spawn(connection.query_outcomes("PROBE"), {.cancel = cancellation.token()});
    check(job.has_value());
    check(co_await markers.receive() == 1);
    auto result = co_await weave::as_result(std::move(*job));
    check(!result && result.error() == std::errc::operation_canceled && !connection.open());
    check(pg::status_name(result.error()) == "canceled");
    failure_snapshot(connection, result.error());
    co_return;
  }
  auto result = co_await weave::as_result(connection.query_outcomes("PROBE"));
  if (mode != "normal") {
    check(!result && !connection.open());
    auto snapshot = failure_snapshot(connection, result.error());
    auto closed = co_await weave::as_result(connection.query_outcomes("must_not_send"));
    check(!closed && closed.error() == pg::Error::closed);
    failure_snapshot(connection, snapshot.error);
    if (mode == "oversized" || mode == "retained_resource")
      check(result.error() == pg::Error::resource_limit);
    else if (mode == "unexpected_copy")
      check(result.error() == pg::Error::unexpected_copy);
    else if (mode != "eof")
      check(result.error() == pg::Error::protocol);
    if (mode == "retained_resource")
      check(connection.last_error().message().size() == 3500);
    if (mode == "oversized" || mode == "retained_resource")
      check(pg::status_name(result.error()) == "resource_limit");
    else if (mode == "unexpected_copy")
      check(pg::status_name(result.error()) == "unexpected_copy");
    else if (mode != "eof")
      check(pg::status_name(result.error()) == "protocol_error");
    co_return;
  }
  check(result.has_value());
  outcomes(*result);
  auto recorded = failure_snapshot(connection, pg::sql_error("22012"));
  auto retained = *result;
  auto reusable = co_await connection.query_outcomes("NOOP");
  check(reusable.size() == 1 && reusable[0].result->kind == pg::ResultKind::command);
  check(connection.last_error().fields.empty());
  failure_snapshot(connection, {});
  check(recorded.error == pg::sql_error("22012") && recorded.diagnostic.message() == "retained failure");
  co_await connection.finish();
  outcomes(retained);
}

int main(int argc, char **argv)
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  std::printf("OpenSSL: %s\n", OpenSSL_version(OPENSSL_VERSION));
  if (argc == 2 && std::string_view{argv[1]} == "format") {
    formatting();
    std::printf("Failure formatting passed: %u checks\n", checks.load());
    return 0;
  }
  if (argc != 3)
    return 1;
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 4096;
  options.limits.result_bytes = 4096;
  std::string mode = argv[2];
  auto separator = mode.find('@');
  auto scenario = separator == std::string::npos ? mode : mode.substr(0, separator);
  auto engine = separator == std::string::npos ? std::string_view{} : std::string_view{mode}.substr(separator + 1);
  if (scenario == "copy_write") {
    options.limits.message_bytes = 16 * 1024 * 1024;
    options.limits.result_bytes = options.limits.message_bytes;
  }
  if (mode == "blocking" || mode.starts_with("blocking_")) {
    auto connection = pg::BlockingConnection::connect(options);
    check(connection.has_value());
    failure_snapshot(*connection, {});
    {
      auto pipeline = connection->pipeline();
      check(pipeline.has_value());
      auto rejected_password = connection->password_verifier("user", "password");
      check(!rejected_password && rejected_password.error() == pg::Error::busy);
      auto rejected_change = connection->change_password("user", "password");
      check(!rejected_change && rejected_change.error() == pg::Error::busy);
      check(pipeline->finish().has_value());
    }
    failure_snapshot(*connection, {});
    auto results = connection->query_outcomes("PROBE");
    if (mode != "blocking") {
      check(!results && !connection->open());
      auto snapshot = failure_snapshot(*connection, results.error());
      auto closed = connection->query_outcomes("must_not_send");
      check(!closed && closed.error() == pg::Error::closed);
      failure_snapshot(*connection, snapshot.error);
      auto moved = std::move(*connection);
      auto missing = connection->last_failure();
      check(!missing && missing.error() == pg::Error::closed);
      failure_snapshot(moved, snapshot.error);
      if (mode == "blocking_duplicate_error")
        check(snapshot.error == pg::Error::protocol);
      std::printf("Blocking failure controls passed: %u checks\n", checks.load());
      return 0;
    }
    check(results.has_value());
    outcomes(*results);
    failure_snapshot(*connection, pg::sql_error("22012"));
    check(connection->query_outcomes("NOOP").has_value());
    check(connection->finish().has_value());
    outcomes(*results);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  else if (!engine.empty()) {
    weave::RuntimeOptions configuration{
      .workers = 4,
      .scheduler = engine.ends_with("affine") ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing,
      .io_layout = engine.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded};
    std::printf(
      "Runtime: workers=4 scheduler=%s io=%s roots=32\n",
      configuration.scheduler == weave::Scheduler::worker_affine ? "affine" : "stealing",
      configuration.io_layout == weave::IoLayout::shared ? "shared" : "sharded");
    auto runtime = weave::Runtime::create(configuration);
    check(runtime.has_value());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn([options, scenario](weave::Context &ctx) {
        return exercise(ctx, options, scenario);
      });
      check(job.has_value());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  else {
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    auto result = ctx->run(weave::timeout(10s, exercise(*ctx, options, mode)));
    if (!result)
      return weave::report_error(result.error());
  }
  std::printf("Failure session controls passed: %u checks\n", checks.load());
}
