#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <source_location>

namespace pg = weave::pg;

static weave::Task<void> check(bool ok, std::source_location location = std::source_location::current())
{
  if (!ok) {
    WEAVE_LOG_ERROR("Encoding check %u", location.line());
    co_await weave::fail(std::errc::bad_message);
  }
}

static weave::Task<void> exercise(pg::Options options, std::string mode)
{
  auto connection = co_await pg::connect(options);
  auto initial = connection.client_encoding();
  if (mode == "unknown" || mode == "absent") {
    auto expected = mode == "unknown" ? std::make_error_code(std::errc::operation_not_supported)
                                      : pg::make_error_code(pg::Error::protocol);
    co_await check(!initial && initial.error() == expected);
    auto quote = connection.escape_literal("x");
    co_await check(!quote && quote.error() == expected && connection.open());
    co_await connection.set_client_encoding(pg::Encoding::utf8);
    co_await check(connection.client_encoding() == pg::Encoding::utf8);
    co_return;
  }
  co_await check(initial == pg::Encoding::utf8);
  if (mode == "cancel") {
    auto cancelled = co_await weave::as_result(
      weave::timeout(std::chrono::milliseconds{50}, connection.set_client_encoding(pg::Encoding::latin1)));
    co_await check(!cancelled && cancelled.error() == std::errc::timed_out && !connection.open());
    co_return;
  }
  auto invalid = co_await weave::as_result(connection.set_client_encoding(static_cast<pg::Encoding>(255)));
  co_await check(!invalid && invalid.error() == std::errc::invalid_argument && connection.open());
  {
    auto deferred = connection.set_client_encoding(pg::Encoding::latin1);
    auto blocked = co_await weave::as_result(connection.reset(options));
    co_await check(!blocked && blocked.error() == pg::Error::busy && connection.open());
    co_await check(!connection.on_notice({}));
  }
  auto changed = co_await weave::as_result(connection.set_client_encoding(pg::Encoding::latin1));
  if (mode == "missing" || mode == "wrong") {
    co_await check(!changed && changed.error() == pg::Error::protocol && !connection.open());
    co_return;
  }
  if (mode == "sql_error") {
    co_await check(!changed && pg::sqlstate(changed.error()) == "0A000" && connection.open());
    co_await check(connection.client_encoding() == pg::Encoding::utf8);
    co_await connection.query("NOOP");
    co_return;
  }
  co_await check(changed.has_value() && connection.client_encoding() == pg::Encoding::latin1);
  co_await check(connection.escape_literal("\xe9'\\") == "E'\xe9''\\\\'");
  co_await connection.query("SET_DIRECT");
  co_await check(connection.client_encoding() == pg::Encoding::sjis);
  co_await check(connection.escape_literal("\x83\x5c") == "E'\x83\x5c'");
  co_await connection.query("SET_UNKNOWN");
  co_await check(!connection.client_encoding() && !connection.escape_identifier("x"));
  co_await connection.set_client_encoding(pg::Encoding::utf8);
  co_await check(connection.client_encoding() == pg::Encoding::utf8);
  {
    auto pipeline = connection.pipeline();
    co_await check(pipeline.has_value());
    auto blocked = co_await weave::as_result(connection.set_client_encoding(pg::Encoding::latin1));
    co_await check(!blocked && blocked.error() == pg::Error::busy);
    co_await check(pipeline->finish().has_value());
  }
  co_await connection.reset(options);
  co_await check(connection.client_encoding() == pg::Encoding::utf8);
  co_await connection.finish();
  auto closed = connection.client_encoding();
  co_await check(!closed && closed.error() == pg::Error::closed);
}

static weave::Result<void> blocking(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(options);
  if (!connection)
    return std::unexpected(connection.error());
  if (connection->client_encoding() != pg::Encoding::utf8 || !connection->set_client_encoding(pg::Encoding::latin1) ||
    connection->client_encoding() != pg::Encoding::latin1 || connection->escape_literal("\xe9") != "E'\xe9'" ||
    !connection->reset(options) || connection->client_encoding() != pg::Encoding::utf8)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  return {};
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 1;
  auto port = weave::parse_port(argv[1]);
  if (!port)
    return 1;
  std::string mode = argv[2];
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  if (mode == "blocking") {
    auto result = blocking(options);
    return result ? 0 : weave::report_error(result.error());
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  if (mode.ends_with("affine") || mode.ends_with("stealing")) {
    auto runtime = weave::Runtime::create(
      {.workers = 4,
        .scheduler = mode.ends_with("affine") ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing,
        .io_layout = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded});
    if (!runtime)
      return weave::report_error(runtime.error());
    std::vector<weave::JoinHandle<void>> jobs;
    for (weave::u32 index = 0; index < 32; ++index) {
      auto job = runtime->spawn(exercise(options, "normal"));
      if (!job)
        return weave::report_error(job.error());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
    return 0;
  }
#endif
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  auto result = ctx->run(exercise(options, mode));
  return result ? 0 : weave::report_error(result.error());
}
