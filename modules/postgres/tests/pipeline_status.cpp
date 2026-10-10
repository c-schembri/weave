#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <openssl/crypto.h>

namespace pg = weave::pg;
static std::atomic<unsigned> checks = 0;

template <class T>
static void check(const T &value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Pipeline status check failed: %u\n", where.line());
    std::_Exit(1);
  }
}

template <class C>
static void state(
  C &connection,
  pg::PipelineStatus expected,
  std::source_location where = std::source_location::current())
{
  check(connection.pipeline_status() == expected, where);
}

template <class C, class P>
static void observe(C &connection, P *&active, unsigned &callbacks)
{
  check(connection
      .on_trace(
        {.handler =
            [&](const pg::TraceMessage &) noexcept {
              auto expected = pg::PipelineStatus::off;
              if (active)
                expected = active->aborted() ? pg::PipelineStatus::aborted : pg::PipelineStatus::on;
              state(connection, expected);
              ++callbacks;
            }})
      .has_value());
}

static weave::Task<void> session(weave::Context &ctx, pg::Options options, std::string_view mode)
{
  auto connection = co_await pg::connect(options);
  state(connection, pg::PipelineStatus::off);
  {
    auto deferred = connection.query("NEVER");
    state(connection, pg::PipelineStatus::off);
  }
  {
    auto unsent = connection.pipeline();
    check(unsent && unsent->execute({.sql = "NEVER"}));
    state(connection, pg::PipelineStatus::on);
    check(connection.transaction() == pg::Transaction::in_progress);
  }
  state(connection, pg::PipelineStatus::off);
  check(connection.open() && connection.transaction() == pg::Transaction::idle);

  pg::Pipeline *active = nullptr;
  unsigned callbacks = 0;
  observe(connection, active, callbacks);
  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    state(connection, pg::PipelineStatus::on);
    check(connection.transaction() == pg::Transaction::idle);
    auto moved = std::move(*pipeline);
    state(connection, pg::PipelineStatus::on);
    check(moved.finish().has_value());
    state(connection, pg::PipelineStatus::off);
  }

  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    active = &*pipeline;
    check(pipeline->execute({.sql = "ERROR"}) && pipeline->execute({.sql = "NOOP"}));
    state(connection, pg::PipelineStatus::on);
    co_await pipeline->flush();
    state(connection, pg::PipelineStatus::aborted);
    check(pipeline->aborted() && connection.open());
    check(connection.transaction() == pg::Transaction::in_progress);
    auto blocked = pipeline->finish();
    check(!blocked && blocked.error() == pg::Error::busy);
    auto rejected = co_await weave::as_result(connection.reset(options));
    check(!rejected && rejected.error() == pg::Error::busy);
    state(connection, pg::PipelineStatus::aborted);
    {
      auto deferred = pipeline->flush();
      state(connection, pg::PipelineStatus::aborted);
    }
    check(pipeline->sync().has_value());
    state(connection, pg::PipelineStatus::aborted);
    co_await pipeline->flush();
    state(connection, pg::PipelineStatus::on);
    check(!pipeline->aborted() && connection.transaction() == pg::Transaction::idle);
    auto error = co_await pipeline->next();
    check(error && error->outcome.error.sqlstate() == "22012");
    check((co_await pipeline->next())->outcome.aborted);
    check((co_await pipeline->next())->transaction == pg::Transaction::idle);
    check(!(co_await pipeline->next()));
    state(connection, pg::PipelineStatus::on);
    check(pipeline->finish().has_value());
    active = nullptr;
    state(connection, pg::PipelineStatus::off);
  }

  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    active = &*pipeline;
    check(pipeline->execute({.sql = "BEGIN"}) && pipeline->execute({.sql = "ERROR"}));
    check(pipeline->execute({.sql = "NOOP"}) && pipeline->sync());
    co_await pipeline->flush();
    state(connection, pg::PipelineStatus::on);
    check(connection.transaction() == pg::Transaction::failed && !pipeline->aborted());
    for (unsigned index = 0; index < 3; ++index)
      check((co_await pipeline->next()).has_value());
    check((co_await pipeline->next())->transaction == pg::Transaction::failed);
    check(!(co_await pipeline->next()));
    check(pipeline->finish().has_value());
    active = nullptr;
  }
  state(connection, pg::PipelineStatus::off);
  co_await connection.query("ROLLBACK");
  check(connection.transaction() == pg::Transaction::idle);

  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    active = &*pipeline;
    check(pipeline->execute({.sql = "NOOP"}) && pipeline->sync());
    check(pipeline->execute({.sql = "NOOP"}) && pipeline->sync());
    co_await weave::when_all(pipeline->receive(), pipeline->send());
    state(connection, pg::PipelineStatus::on);
    for (unsigned index = 0; index < 4; ++index)
      check((co_await pipeline->next()).has_value());
    check(!(co_await pipeline->next()));
    check(pipeline->finish().has_value());
    active = nullptr;
  }
  check(callbacks > 0);

  if (mode == "normal") {
    check(connection.on_trace({}).has_value());
    co_await connection.reset(options);
    state(connection, pg::PipelineStatus::off);
    auto moved = std::move(connection);
    state(connection, pg::PipelineStatus::off);
    state(moved, pg::PipelineStatus::off);
    co_await moved.finish();
    state(moved, pg::PipelineStatus::off);
    co_return;
  }

  weave::CancelSource cancellation;
  if (mode == "cancel") {
    check(connection
        .on_notice([&](const pg::Diagnostic &notice) noexcept {
          check(notice.message() == "PENDING");
          state(connection, pg::PipelineStatus::on);
          cancellation.cancel();
        })
        .has_value());
  }

  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    active = &*pipeline;
    if (mode == "cancel") {
      check(pipeline->execute({.sql = "WAIT"}));
      auto job = ctx.spawn(pipeline->flush(), {.cancel = cancellation.token()});
      check(job.has_value());
      auto failed = co_await weave::as_result(std::move(*job));
      check(!failed && failed.error() == std::errc::operation_canceled && !connection.open());
      state(connection, pg::PipelineStatus::on);
      check(!pipeline->aborted());
    } else {
      check(pipeline->execute({.sql = mode == "drop" ? "NOOP" : mode == "bad" ? "BAD" : "EOF"}));
      auto flushed = co_await weave::as_result(pipeline->flush());
      check(flushed.has_value() == (mode == "drop"));
      state(connection, pg::PipelineStatus::on);
      check(!pipeline->aborted());
    }
  }
  active = nullptr;
  state(connection, pg::PipelineStatus::off);
  check(!connection.open() && connection.transaction() == pg::Transaction::unknown);
  if (mode == "cancel")
    check(connection.on_notice({}).has_value());
}

static void blocking(pg::Options options, std::string_view mode)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection.has_value());
  state(*connection, pg::PipelineStatus::off);
  {
    auto unsent = connection->pipeline();
    check(unsent && unsent->execute({.sql = "NEVER"}));
    state(*connection, pg::PipelineStatus::on);
  }
  state(*connection, pg::PipelineStatus::off);
  check(connection->open() && connection->transaction() == pg::Transaction::idle);
  pg::BlockingPipeline *active = nullptr;
  unsigned callbacks = 0;
  observe(*connection, active, callbacks);

  {
    auto pipeline = connection->pipeline();
    check(pipeline.has_value());
    state(*connection, pg::PipelineStatus::on);
    check(connection->transaction() == pg::Transaction::idle);
    auto moved = std::move(*pipeline);
    check(moved.finish());
    state(*connection, pg::PipelineStatus::off);
  }
  {
    auto pipeline = connection->pipeline();
    check(pipeline.has_value());
    active = &*pipeline;
    check(pipeline->execute({.sql = "ERROR"}) && pipeline->execute({.sql = "NOOP"}));
    check(pipeline->flush());
    state(*connection, pg::PipelineStatus::aborted);
    check(pipeline->aborted() && connection->open());
    check(!pipeline->finish() && !connection->reset(options));
    check(pipeline->sync());
    state(*connection, pg::PipelineStatus::aborted);
    check(pipeline->flush());
    state(*connection, pg::PipelineStatus::on);
    auto error = pipeline->next();
    check(error && *error && (**error).outcome.error.sqlstate() == "22012");
    auto skipped = pipeline->next();
    check(skipped && *skipped && (**skipped).outcome.aborted);
    auto sync = pipeline->next();
    check(sync && *sync && (**sync).transaction == pg::Transaction::idle);
    check(pipeline->finish());
    active = nullptr;
  }
  {
    auto pipeline = connection->pipeline();
    check(pipeline.has_value());
    active = &*pipeline;
    check(pipeline->execute({.sql = "BEGIN"}) && pipeline->execute({.sql = "ERROR"}));
    check(pipeline->execute({.sql = "NOOP"}) && pipeline->sync());
    check(pipeline->flush());
    state(*connection, pg::PipelineStatus::on);
    check(connection->transaction() == pg::Transaction::failed && !pipeline->aborted());
    for (unsigned index = 0; index < 4; ++index) {
      auto next = pipeline->next();
      check(next && *next);
    }
    check(pipeline->finish());
    active = nullptr;
  }
  check(connection->query("ROLLBACK"));
  state(*connection, pg::PipelineStatus::off);
  check(callbacks > 0);

  if (mode == "normal") {
    check(connection->on_trace({}));
    check(connection->reset(options));
    auto moved = std::move(*connection);
    state(*connection, pg::PipelineStatus::off);
    state(moved, pg::PipelineStatus::off);
    check(moved.finish());
    state(moved, pg::PipelineStatus::off);
    return;
  }
  {
    auto pipeline = connection->pipeline();
    check(pipeline.has_value());
    active = &*pipeline;
    if (mode == "cancel") {
      check(connection->cancel());
    } else {
      check(pipeline->execute({.sql = mode == "drop" ? "NOOP" : mode == "bad" ? "BAD" : "EOF"}));
      auto flushed = pipeline->flush();
      check(flushed.has_value() == (mode == "drop"));
    }
    state(*connection, pg::PipelineStatus::on);
    check(!pipeline->aborted());
  }
  active = nullptr;
  state(*connection, pg::PipelineStatus::off);
  check(!connection->open());
}

int main(int argc, char **argv)
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  if (argc != 4)
    return 1;
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  std::string_view engine = argv[2], mode = argv[3];

  if (engine == "blocking") {
    blocking(options, mode);
  } else if (engine == "context") {
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    auto result = ctx->run(session(*ctx, options, mode));
    if (!result)
      return weave::report_error(result.error());
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  else {
    auto runtime = weave::Runtime::create(
      {.workers = 4,
        .scheduler = engine.ends_with("affine") ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing,
        .io_layout = engine.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded});
    check(runtime.has_value());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned root = 0; root < 16; ++root) {
      auto job = runtime->spawn([options, mode](weave::Context &ctx) {
        return session(ctx, options, mode);
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
#else
  else {
    return 1;
  }
#endif
  std::printf(
    "Pipeline status controls passed: %u checks; OpenSSL: %s\n",
    checks.load(),
    OpenSSL_version(OPENSSL_VERSION));
}
