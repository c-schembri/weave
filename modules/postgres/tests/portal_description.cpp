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
    std::fprintf(stderr, "Portal session check failed at line %u\n", location.line());
    std::exit(1);
  }
}

static void columns(const pg::ResultSet &result)
{
  check(result.columns.size() == 1 && result.rows.empty() && result.parameter_types.empty());
  check(result.command.empty() && !result.suspended);
  const auto &column = result.columns.front();
  check(column.name == "value" && column.table == 16384 && column.attribute == 2);
  check(column.type == 23 && column.type_size == 4 && column.modifier == -1 && column.format == pg::Format::binary);
}

static weave::Task<void> exercise(weave::Context &ctx, pg::Options options, std::string mode)
{
  auto connection = co_await pg::connect(options);
  auto invalid = co_await weave::as_result(connection.describe_portal(std::string{"bad\0name", 8}));
  check(!invalid && invalid.error() == std::errc::invalid_argument && connection.open());
  auto large = co_await weave::as_result(connection.describe_portal(std::string(1024, 'p')));
  check(!large && large.error() == pg::Error::resource_limit && connection.open());
  {
    auto dropped = connection.describe_portal("must_not_send");
    auto blocked = co_await weave::as_result(connection.reset(options));
    check(!blocked && blocked.error() == pg::Error::busy && connection.open());
    auto info = connection.info();
    check(!info && info.error() == pg::Error::busy);
  }
  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    auto blocked = co_await weave::as_result(connection.describe_portal("must_not_send"));
    check(!blocked && blocked.error() == pg::Error::busy && connection.open());
    check(pipeline->finish().has_value());
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
    auto job = ctx.spawn(connection.describe_portal("probe"), {.cancel = cancellation.token()});
    check(job.has_value());
    check(co_await markers.receive() == 1);
    while (ctx.metrics().submitted == ctx.metrics().completed)
      co_await ctx.yield();
    auto pending = ctx.metrics();
    check(pending.submitted == pending.completed + 1);
    auto competing = co_await weave::as_result(connection.query("must_not_send"));
    check(!competing && competing.error() == pg::Error::busy);
    cancellation.cancel();
    auto result = co_await weave::as_result(std::move(*job));
    check(!result && result.error() == std::errc::operation_canceled && !connection.open());
    auto drained = ctx.metrics();
    check(drained.submitted == drained.completed);
    co_return;
  }

  auto result = co_await weave::as_result(connection.describe_portal("probe"));
  if (mode == "sql_error") {
    check(!result && pg::sqlstate(result.error()) == "34000" && connection.open());
    check(connection.last_error().sqlstate() == "34000");
    auto reusable = co_await connection.query("NOOP");
    check(reusable.size() == 1 && reusable[0].command == "SELECT 1");
    co_await connection.finish();
    co_return;
  }
  if (mode != "normal") {
    auto expected = mode == "oversized" ? pg::Error::resource_limit : pg::Error::protocol;
    check(!result && result.error() == expected && !connection.open());
    co_return;
  }
  check(result.has_value());
  columns(*result);
  auto retained = *result;
  auto empty = co_await connection.describe_portal("");
  check(empty.columns.empty() && empty.rows.empty() && empty.parameter_types.empty() && empty.command.empty());
  auto reusable = co_await connection.query("NOOP");
  check(reusable.size() == 1 && reusable[0].command == "SELECT 1");
  co_await connection.finish();
  columns(retained);
  auto closed = co_await weave::as_result(connection.describe_portal("probe"));
  check(!closed && closed.error() == pg::Error::closed);
}

static weave::Result<void> blocking(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection.has_value());
  auto invalid = connection->describe_portal(std::string{"bad\0name", 8});
  check(!invalid && invalid.error() == std::errc::invalid_argument);
  auto result = connection->describe_portal("probe");
  check(result.has_value());
  columns(*result);
  auto empty = connection->describe_portal("");
  check(empty && empty->columns.empty() && empty->parameter_types.empty());
  auto reusable = connection->query("NOOP");
  check(reusable && reusable->size() == 1);
  auto finished = connection->finish();
  if (!finished)
    return std::unexpected(finished.error());
  columns(*result);
  return {};
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 1;
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 1024;
  std::string mode = argv[2];
  if (mode == "blocking") {
    auto result = blocking(options);
    if (!result)
      return weave::report_error(result.error());
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
  std::printf("Portal session controls passed: %u checks\n", checks.load());
}
