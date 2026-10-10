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

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Transaction check failed: %u\n", where.line());
    std::_Exit(1);
  }
}

template <class C>
static void state(C &connection, pg::Transaction expected, std::source_location where = std::source_location::current())
{
  check(connection.transaction() == expected, where);
}

template <class C>
static void observe(C &connection, unsigned &writes, unsigned &notices, unsigned &syncs)
{
  check(connection
      .on_trace(
        {.handler =
            [&](const pg::TraceMessage &message) noexcept {
              if (message.direction == pg::TraceDirection::frontend && message.kind != 'X') {
                state(connection, pg::Transaction::in_progress);
                ++writes;
              }
              if (message.direction == pg::TraceDirection::backend && message.kind == 'Z') {
                state(connection, pg::Transaction::in_progress);
                ++syncs;
              }
              if (message.direction == pg::TraceDirection::backend && message.kind == '1' && syncs) {
                state(connection, pg::Transaction::in_progress);
              }
            }})
      .has_value());
  check(connection
      .on_notice([&](const pg::Diagnostic &) noexcept {
        state(connection, pg::Transaction::in_progress);
        ++notices;
      })
      .has_value());
  check(connection
      .on_notification([&](const pg::Notification &) noexcept {
        state(connection, pg::Transaction::idle);
      })
      .has_value());
}

static weave::Task<void> session(pg::Options options, std::string_view mode)
{
  auto connection = co_await pg::connect(options);
  state(connection, pg::Transaction::idle);
  check(connection.info()->transaction == pg::Transaction::idle);
  {
    auto deferred = connection.query("NEVER");
    state(connection, pg::Transaction::idle);
  }
  {
    auto deferred = connection.reset(options);
    state(connection, pg::Transaction::idle);
  }
  unsigned writes = 0, notices = 0, syncs = 0;
  observe(connection, writes, notices, syncs);
  co_await connection.query("BEGIN");
  state(connection, pg::Transaction::active);
  auto failed = co_await weave::as_result(connection.query("ERROR"));
  check(!failed && pg::sqlstate(failed.error()) == "22012" && connection.open());
  state(connection, pg::Transaction::failed);
  co_await connection.query("ROLLBACK");
  state(connection, pg::Transaction::idle);
  co_await connection.query("NOTICE");
  check(notices == 1);
  co_await connection.query("NOOP");
  state(connection, pg::Transaction::idle);
  co_await connection.wait_notification();
  state(connection, pg::Transaction::idle);

  co_await connection.start_rows("ROWS");
  state(connection, pg::Transaction::in_progress);
  check(connection.info()->transaction == pg::Transaction::in_progress);
  auto row = co_await connection.read_row();
  check(row && row->size() == 1);
  state(connection, pg::Transaction::in_progress);
  check(!(co_await connection.read_row()));
  state(connection, pg::Transaction::idle);

  co_await connection.start_copy("COPY");
  state(connection, pg::Transaction::in_progress);
  check(connection.info()->transaction == pg::Transaction::in_progress);
  check((co_await connection.read_copy()).has_value());
  state(connection, pg::Transaction::in_progress);
  check(!(co_await connection.read_copy()));
  state(connection, pg::Transaction::idle);
  check(connection.copy_result().has_value());

  {
    auto exchange = co_await connection.exchange("EXCHANGE");
    state(connection, pg::Transaction::in_progress);
    check((co_await exchange.next()).has_value());
    state(connection, pg::Transaction::in_progress);
    check(!(co_await exchange.next()));
    state(connection, pg::Transaction::idle);
    check(exchange.finish().has_value());
  }

  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    state(connection, pg::Transaction::idle);
    check(pipeline->execute({.sql = "NOOP"}).has_value());
    state(connection, pg::Transaction::in_progress);
    co_await pipeline->flush();
    state(connection, pg::Transaction::in_progress);
    check(pipeline->sync().has_value());
    co_await pipeline->flush();
    state(connection, pg::Transaction::idle);
    check((co_await pipeline->next()).has_value());
    check((co_await pipeline->next())->transaction == pg::Transaction::idle);
    check(!(co_await pipeline->next()));
    check(pipeline->finish().has_value());
  }
  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    check(pipeline->execute({.sql = "NOOP"}).has_value() && pipeline->sync().has_value());
    check(pipeline->execute({.sql = "NOOP"}).has_value() && pipeline->sync().has_value());
    co_await weave::when_all(pipeline->receive(), pipeline->send());
    state(connection, pg::Transaction::idle);
    for (unsigned index = 0; index < 4; ++index)
      check((co_await pipeline->next()).has_value());
    check(!(co_await pipeline->next()));
    check(pipeline->finish().has_value());
  }
  check(writes > 0 && syncs > 0);
  check(connection.on_trace({}).has_value() && connection.on_notice({}).has_value());

  if (mode == "normal") {
    co_await connection.reset(options);
    state(connection, pg::Transaction::idle);
    auto moved = std::move(connection);
    state(connection, pg::Transaction::unknown);
    co_await moved.finish();
    state(moved, pg::Transaction::unknown);
  } else if (mode == "cancel") {
    check(connection.cancel().has_value());
    state(connection, pg::Transaction::unknown);
  } else {
    auto result = co_await weave::as_result(connection.query(mode == "bad" ? "BAD" : "EOF"));
    check(!result && !connection.open());
    state(connection, pg::Transaction::unknown);
    check(connection.configuration().has_value());
  }
}

static void blocking(pg::Options options, std::string_view mode)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection.has_value());
  state(*connection, pg::Transaction::idle);
  check(connection->info()->transaction == pg::Transaction::idle);
  unsigned writes = 0, notices = 0, syncs = 0;
  observe(*connection, writes, notices, syncs);
  check(connection->query("BEGIN").has_value());
  state(*connection, pg::Transaction::active);
  auto failed = connection->query("ERROR");
  check(!failed && pg::sqlstate(failed.error()) == "22012" && connection->open());
  state(*connection, pg::Transaction::failed);
  check(connection->query("ROLLBACK").has_value());
  state(*connection, pg::Transaction::idle);
  check(connection->query("NOTICE").has_value() && notices == 1);
  check(connection->query("NOOP").has_value());
  state(*connection, pg::Transaction::idle);
  check(connection->wait_notification().has_value());
  state(*connection, pg::Transaction::idle);

  check(connection->start_rows("ROWS").has_value());
  state(*connection, pg::Transaction::in_progress);
  check(connection->info()->transaction == pg::Transaction::in_progress);
  auto row = connection->read_row();
  check(row && *row && (**row).size() == 1);
  state(*connection, pg::Transaction::in_progress);
  auto end = connection->read_row();
  check(end && !*end);
  state(*connection, pg::Transaction::idle);

  check(connection->start_copy("COPY").has_value());
  state(*connection, pg::Transaction::in_progress);
  check(connection->info()->transaction == pg::Transaction::in_progress);
  auto copied = connection->read_copy();
  check(copied && *copied);
  state(*connection, pg::Transaction::in_progress);
  copied = connection->read_copy();
  check(copied && !*copied);
  state(*connection, pg::Transaction::idle);
  check(connection->copy_result().has_value());
  {
    auto exchange = connection->exchange("EXCHANGE");
    check(exchange.has_value());
    state(*connection, pg::Transaction::in_progress);
    auto next = exchange->next();
    check(next && *next);
    state(*connection, pg::Transaction::in_progress);
    next = exchange->next();
    check(next && !*next);
    state(*connection, pg::Transaction::idle);
    check(exchange->finish().has_value());
  }
  {
    auto pipeline = connection->pipeline();
    check(pipeline.has_value());
    state(*connection, pg::Transaction::idle);
    check(pipeline->execute({.sql = "NOOP"}).has_value());
    state(*connection, pg::Transaction::in_progress);
    check(pipeline->flush().has_value());
    state(*connection, pg::Transaction::in_progress);
    check(pipeline->sync().has_value() && pipeline->flush().has_value());
    state(*connection, pg::Transaction::idle);
    for (unsigned index = 0; index < 2; ++index) {
      auto next = pipeline->next();
      check(next && *next);
    }
    check(pipeline->finish().has_value());
  }
  check(writes > 0 && syncs > 0);
  check(connection->on_trace({}).has_value() && connection->on_notice({}).has_value());
  if (mode == "normal") {
    check(connection->reset(options).has_value());
    state(*connection, pg::Transaction::idle);
    auto moved = std::move(*connection);
    state(*connection, pg::Transaction::unknown);
    check(moved.finish().has_value());
    state(moved, pg::Transaction::unknown);
  } else if (mode == "cancel") {
    check(connection->cancel().has_value());
    state(*connection, pg::Transaction::unknown);
  } else {
    auto result = connection->query(mode == "bad" ? "BAD" : "EOF");
    check(!result && !connection->open());
    state(*connection, pg::Transaction::unknown);
  }
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
    auto result = ctx->run(session(options, mode));
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
      auto job = runtime->spawn(session(options, mode));
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
  std::printf("Transaction controls passed: %u checks; OpenSSL: %s\n", checks.load(), OpenSSL_version(OPENSSL_VERSION));
}
