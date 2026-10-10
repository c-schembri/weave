#include <weave/postgres.hpp>
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <weave/timer.hpp>
#include <array>
#include <cstdio>

namespace pg = weave::pg;
using namespace std::chrono_literals;

static weave::Task<void> check(bool condition)
{
  if (!condition)
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> send(pg::Exchange &exchange)
{
  std::vector<std::byte> bytes(1024 * 1024, std::byte{'y'});
  for (unsigned index = 0; index < 4; ++index)
    co_await exchange.write(bytes);
  co_await exchange.finish_send();
}

static weave::Task<void> receive(pg::Exchange &exchange)
{
  std::size_t bytes = 0;
  std::size_t commands = 0;
  bool done = false;
  while (auto event = co_await exchange.next()) {
    if (auto data = std::get_if<std::vector<std::byte>>(&*event)) {
      co_await check(pg::status_name(*event) == "copy_data");
      co_await check(data->size() == 1024 * 1024 && data->front() == std::byte{'x'});
      bytes += data->size();
    } else if (std::holds_alternative<pg::CopyDone>(*event)) {
      co_await check(pg::status_name(*event) == "copy_done");
      co_await check(!done);
      done = true;
    } else if (std::holds_alternative<pg::ResultSet>(*event)) {
      co_await check(pg::status_name(*event) == "command");
      co_await check(done);
      ++commands;
    } else {
      co_await check(false);
    }
  }
  co_await check(bytes == 4 * 1024 * 1024 && done && commands == 2);
}

static weave::Task<void> probe(pg::Options options, std::string mode)
{
  auto connection = co_await pg::connect(options);
  auto exchange = co_await connection.exchange("probe");
  if (mode == "abandon") {
    auto event = co_await exchange.next();
    co_await check(event && std::holds_alternative<pg::CopyFormat>(*event));
    co_return;
  }
  if (mode == "duplex") {
    auto event = co_await exchange.next();
    co_await check(event && std::get<pg::CopyFormat>(*event).direction == pg::CopyDirection::both);
    co_await check(pg::status_name(*event) == "copy_both");
    co_await weave::when_all(send(exchange), receive(exchange));
    if (auto status = exchange.finish(); !status)
      co_await weave::fail(status.error());
    co_await connection.finish();
    co_return;
  }

  std::optional<weave::Task<void>> deferred;
  std::size_t results = 0;
  std::size_t chunks = 0;
  std::size_t ends = 0;
  bool empty_chunk = false;
  std::error_code failure;
  for (;;) {
    auto timeout = mode == "cancel_header" || mode == "cancel_read";
    auto operation = timeout ? weave::timeout(20ms, exchange.next()) : exchange.next();
    auto event = co_await weave::as_result(std::move(operation));
    if (!event) {
      failure = event.error();
      break;
    }
    if (!*event)
      break;
    if (auto format = std::get_if<pg::CopyFormat>(&**event)) {
      co_await check(pg::status_name(**event) == pg::status_name(*format));
      if (mode == "input_deferred") {
        deferred.emplace(exchange.write({}));
      } else if (format->direction != pg::CopyDirection::output) {
        if (mode == "input_error")
          co_await exchange.finish_send("fixture rejection");
        else
          co_await exchange.finish_send();
      }
    } else if (std::holds_alternative<pg::ResultSet>(**event)) {
      co_await check(pg::status_name(**event) == pg::status_name(std::get<pg::ResultSet>(**event)));
      ++results;
    } else if (auto data = std::get_if<std::vector<std::byte>>(&**event)) {
      co_await check(pg::status_name(**event) == "copy_data");
      ++chunks;
      empty_chunk = empty_chunk || data->empty();
    } else {
      co_await check(pg::status_name(**event) == "copy_done");
      ++ends;
    }
  }
  deferred.reset();

  const std::array success_modes{"output", "boundary", "keepalive"};
  bool success = std::ranges::find(success_modes, mode) != success_modes.end();
  bool sql_error = mode == "input_error" || mode == "output_error" || mode == "input_deferred" || mode == "both_error";
  bool reusable_error = mode == "input_error" || mode == "output_error";
  if (success || reusable_error) {
    co_await check(connection.open());
    co_await check(success ? !failure : pg::sqlstate(failure) == "22000");
    if (mode == "output")
      co_await check(results == 2 && chunks == 2 && empty_chunk && ends == 1);
    if (mode == "boundary")
      co_await check(results == 4 && chunks == 1 && ends == 1);
    if (mode == "keepalive")
      co_await check(results == 2 && chunks == 0 && ends == 1);
    if (auto status = exchange.finish(); !status)
      co_await weave::fail(status.error());
    co_await connection.query("reuse");
    co_await connection.finish();
  } else {
    co_await check(failure && !connection.open());
    if (sql_error)
      co_await check(pg::sqlstate(failure) == "22000");
    else if (mode == "cancel_header" || mode == "cancel_read")
      co_await check(failure == std::errc::timed_out);
    else if (mode == "limit")
      co_await check(failure == pg::Error::resource_limit);
    else if (mode != "eof_header" && mode != "eof_body")
      co_await check(failure == pg::Error::protocol);
    auto status = exchange.finish();
    co_await check(!status && status.error() == pg::Error::closed);
  }
}

int main(int argc, char **argv)
{
  if (argc != 3 && argc != 5)
    return 2;
  auto port = weave::parse_port(argv[1]);
  if (!port)
    return 2;
  std::string mode = argv[2];
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "fixture", .plaintext = true};
  options.replication = mode == "boundary" || mode == "keepalive" || mode == "bad_keepalive" || mode == "late_data"
    ? pg::Replication::physical
    : pg::Replication::disabled;
  if (mode == "limit") {
    options.limits.message_bytes = 1024;
    options.limits.result_bytes = 1024;
  }
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  if (argc == 5) {
    auto scheduler = std::string_view{argv[3]} == "stealing" ? weave::Scheduler::work_stealing
                                                             : weave::Scheduler::worker_affine;
    auto layout = std::string_view{argv[4]} == "shared" ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
    if (!runtime)
      return weave::report_error(runtime.error());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn(weave::timeout(10s, probe(options, mode)));
      if (!job)
        return weave::report_error(job.error());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      if (auto result = std::move(job).get(); !result)
        return weave::report_error(result.error());
    }
    std::puts("16 unpinned duplex roots passed");
    return 0;
  }
#endif
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  auto result = ctx->run(weave::timeout(5s, probe(options, mode)));
  if (!result)
    return weave::report_error(result.error());
  auto metrics = ctx->metrics();
  if (metrics.submitted != metrics.completed)
    return 3;
  std::puts("passed");
}
