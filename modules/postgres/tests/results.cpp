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

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks = 0;

static void check(bool condition, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Result session check failed at line %u\n", location.line());
    std::exit(1);
  }
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

static weave::Task<void> exercise(weave::Context &ctx, pg::Options options, std::string mode)
{
  auto connection = co_await pg::connect(options);
  auto invalid = co_await weave::as_result(connection.query_outcomes(std::string{"bad\0sql", 7}));
  check(!invalid && invalid.error() == std::errc::invalid_argument && connection.open());
  {
    auto deferred = connection.query_outcomes("must_not_send");
    auto blocked = co_await weave::as_result(connection.reset(options));
    check(!blocked && blocked.error() == pg::Error::busy && connection.open());
  }
  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    auto blocked = co_await weave::as_result(connection.query_outcomes("must_not_send"));
    check(!blocked && blocked.error() == pg::Error::busy);
    check(pipeline->finish().has_value());
  }
  if (mode.starts_with("extended")) {
    auto result = co_await weave::as_result(connection.execute_outcome("PROBE"));
    if (mode == "extended_error") {
      check(result && !result->result && result->error.sqlstate() == "22012" && connection.open());
      check(pg::status_name(*result) == "sql_error");
      auto retained = *result;
      co_await connection.finish();
      check(retained.error.message() == "retained failure");
    } else {
      check(!result && result.error() == pg::Error::protocol && !connection.open());
      check(pg::status_name(result.error()) == "protocol_error");
    }
    co_return;
  }
  if (mode == "cancel") {
    weave::Channel<int> markers{1};
    check(connection
        .on_notice([&](const pg::Diagnostic &notice) noexcept {
          if (notice.message() == "PENDING") {
            int marker = 1;
            check(markers.try_send(marker).has_value());
          }
        })
        .has_value());
    weave::CancelSource cancellation;
    auto job = ctx.spawn(connection.query_outcomes("PROBE"), {.cancel = cancellation.token()});
    check(job.has_value());
    check(co_await markers.receive() == 1);
    while (ctx.metrics().submitted == ctx.metrics().completed)
      co_await ctx.yield();
    check(ctx.metrics().submitted == ctx.metrics().completed + 1);
    cancellation.cancel();
    auto result = co_await weave::as_result(std::move(*job));
    check(!result && result.error() == std::errc::operation_canceled && !connection.open());
    check(pg::status_name(result.error()) == "canceled");
    check(ctx.metrics().submitted == ctx.metrics().completed);
    co_return;
  }
  auto result = co_await weave::as_result(connection.query_outcomes("PROBE"));
  if (mode != "normal") {
    check(!result && !connection.open());
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
  auto retained = *result;
  auto reusable = co_await connection.query_outcomes("NOOP");
  check(reusable.size() == 1 && reusable[0].result->kind == pg::ResultKind::command);
  check(connection.last_error().fields.empty());
  co_await connection.finish();
  outcomes(retained);
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 1;
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 4096;
  options.limits.result_bytes = 4096;
  std::string mode = argv[2];
  if (mode == "blocking") {
    auto connection = pg::BlockingConnection::connect(options);
    check(connection.has_value());
    auto results = connection->query_outcomes("PROBE");
    check(results.has_value());
    outcomes(*results);
    check(connection->query_outcomes("NOOP").has_value());
    check(connection->finish().has_value());
    outcomes(*results);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  else if (mode.ends_with("affine") || mode.ends_with("stealing")) {
    auto runtime = weave::Runtime::create(
      {.workers = 4,
        .scheduler = mode.ends_with("affine") ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing,
        .io_layout = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded});
    check(runtime.has_value());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn([options](weave::Context &ctx) {
        return exercise(ctx, options, "normal");
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
  std::printf("Result session controls passed: %u checks\n", checks.load());
}
