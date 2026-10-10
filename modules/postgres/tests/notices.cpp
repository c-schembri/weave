#include <weave/postgres.hpp>
#include <weave/io.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <memory>
#include <string>
#include <vector>
#include <source_location>

namespace pg = weave::pg;

static_assert(std::constructible_from<pg::NoticeHandler, decltype([](const pg::Diagnostic &) noexcept {
})>);
static_assert(!std::constructible_from<pg::NoticeHandler, decltype([](const pg::Diagnostic &) {
})>);

static weave::Task<void> notice_check(bool value, std::source_location location = std::source_location::current())
{
  if (!value) {
    WEAVE_LOG_ERROR("Notice fixture: %s:%u", location.file_name(), location.line());
    co_await weave::fail(std::errc::bad_message);
  }
}

struct NoticeLifetime {
  weave::u32 *destroyed;

  ~NoticeLifetime()
  {
    ++*destroyed;
  }
};

static weave::Task<void> observe_notices(pg::Options options)
{
  weave::u32 count = 0;
  weave::u32 destroyed = 0;
  bool guarded = true;
  pg::Connection *active = nullptr;
  std::vector<pg::Diagnostic> retained;
  auto owner = std::make_unique<NoticeLifetime>(&destroyed);
  pg::Diagnostic diagnostic;
  auto connection = co_await pg::connect(
    options,
    diagnostic,
    [capture = std::move(owner), &count, &guarded, &retained, &active](const pg::Diagnostic &notice) noexcept {
      ++count;
      retained.push_back(notice);
      if (active) {
        auto replacement = active->on_notice({});
        auto cancellation = active->cancel();
        auto closure = active->close();
        guarded = guarded && !replacement && replacement.error() == pg::Error::busy && !cancellation &&
          cancellation.error() == pg::Error::busy && !closure && closure.error() == pg::Error::busy;
      }
    });
  active = &connection;
  co_await notice_check(count == 1 && diagnostic.fields.empty() && connection.take_notices().empty());
  co_await connection.query("NOTICES");
  co_await notice_check(count == 5 && guarded && connection.take_notices().empty());
  co_await notice_check(retained.front().message() == "startup" && retained.back().message() == "notice 3");
  {
    auto deferred = connection.query("NOTICES");
    auto rejected = connection.on_notice({});
    co_await notice_check(!rejected && rejected.error() == pg::Error::busy && destroyed == 0);
  }
  {
    auto pipeline = connection.pipeline();
    co_await notice_check(pipeline.has_value());
    co_await notice_check(!connection.on_notice({}));
    co_await notice_check(pipeline->finish().has_value());
  }
  auto sql_error = co_await weave::as_result(connection.query("ERROR"));
  co_await notice_check(!sql_error && pg::sqlstate(sql_error.error()) == "22012" && count == 6);
  auto moved = std::move(connection);
  active = &moved;
  co_await moved.reset(options);
  co_await notice_check(count == 7 && guarded && destroyed == 0 && moved.take_notices().empty());

  auto failed_options = options;
  failed_options.user = "bad";
  auto failed = co_await weave::as_result(moved.reset(failed_options, diagnostic));
  co_await notice_check(!failed && diagnostic.sqlstate() == "28P01" && !moved.open());
  co_await notice_check(count == 8 && guarded && destroyed == 0);
  co_await moved.reset(options);
  co_await notice_check(count == 9 && guarded && destroyed == 0);
  {
    auto deferred = moved.reset(options);
    auto rejected = moved.on_notice({});
    co_await notice_check(!rejected && rejected.error() == pg::Error::busy);
    auto competing = co_await weave::as_result(moved.reset(options));
    co_await notice_check(!competing && competing.error() == pg::Error::busy && moved.open() && count == 9);
  }
  auto previous = moved.on_notice({});
  co_await notice_check(previous && *previous && destroyed == 0);
  co_await moved.query("ONE_NOTICE");
  co_await notice_check(count == 9);
  auto restored = moved.on_notice(std::move(*previous));
  co_await notice_check(restored && !*restored && destroyed == 0);
  co_await moved.query("NOTICES");
  co_await notice_check(count == 13 && guarded);
  auto saved_queue = moved.take_notices();
  co_await notice_check(saved_queue.size() == 1 && saved_queue.front().message() == "notice 0");
  co_await notice_check(moved.on_notice({}).has_value());
  co_await notice_check(destroyed == 1);
  auto overflow = co_await weave::as_result(moved.query("NOTICES"));
  co_await notice_check(!overflow && overflow.error() == pg::Error::resource_limit && !moved.open());
  auto queued = moved.take_notices();
  co_await notice_check(queued.size() == options.limits.queued_notifications && count == 13);
  co_await notice_check(retained.front().message() == "startup");
}

static weave::Task<void> malformed_notices(pg::Options options, bool oversized)
{
  weave::u32 count = 0;
  auto connection = co_await pg::connect(options, [&count](const pg::Diagnostic &) noexcept {
    ++count;
  });
  co_await notice_check(count == 1);
  auto result = co_await weave::as_result(connection.query(oversized ? "OVERSIZED" : "MALFORMED"));
  auto expected = oversized ? pg::Error::resource_limit : pg::Error::protocol;
  co_await notice_check(!result && result.error() == expected && count == 1 && !connection.open());
}

static weave::Result<void> blocking_notices(pg::Options options)
{
  weave::u32 count = 0;
  auto callback = [&count](const pg::Diagnostic &) noexcept {
    ++count;
  };
  auto connection = pg::BlockingConnection::connect(options, callback);
  if (!connection)
    return std::unexpected(connection.error());
  if (count != 1 || !connection->query("NOTICES") || count != 5 || !connection->take_notices().empty())
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto reset = connection->reset(options); !reset)
    return reset;
  if (count != 6)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto installed = connection->on_notice({}); !installed)
    return std::unexpected(installed.error());
  auto result = connection->query("NOTICES");
  if (result || result.error() != pg::Error::resource_limit || count != 6)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  return {};
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 1;
  auto port = weave::parse_port(argv[1]);
  if (!port)
    return weave::report_error(port.error());
  const std::string mode = argv[2];
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  options.limits.message_bytes = 1024;
  options.limits.result_bytes = 4096;
  options.limits.queued_notifications = 2;
  if (mode == "blocking") {
    auto result = blocking_notices(options);
    return result ? 0 : weave::report_error(result.error());
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  if (mode == "affine" || mode == "stealing" || mode == "shared_affine" || mode == "shared_stealing") {
    auto scheduler = mode.ends_with("affine") ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing;
    auto layout = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
    if (!runtime)
      return weave::report_error(runtime.error());
    std::vector<weave::JoinHandle<void>> jobs;
    for (weave::u32 index = 0; index < 16; ++index) {
      auto job = runtime->spawn(observe_notices(options));
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
  if (mode != "observe" && mode != "malformed" && mode != "oversized") {
    WEAVE_LOG_ERROR("Unknown notice fixture mode: %s", mode.c_str());
    return 1;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  auto operation = mode == "observe" ? observe_notices(options) : malformed_notices(options, mode == "oversized");
  auto result = ctx->run(std::move(operation));
  return result ? 0 : weave::report_error(result.error());
}
