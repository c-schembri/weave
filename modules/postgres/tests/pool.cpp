#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include "runtime_fixture.hpp"
#endif
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <atomic>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Pool check failed: %s:%u\n", location.file_name(), location.line());
    std::exit(EXIT_FAILURE);
  }
}

static weave::Task<void> borrower(pg::Pool &pool)
{
  auto database = co_await pool.acquire();
  co_await database->query("NOOP");
  co_await weave::sleep_for(1ms);
}

static weave::Task<void> wait_pending(pg::Pool &pool)
{
  while (pool.stats().pending == 0)
    co_await weave::sleep_for(1ms);
}

static weave::Task<void> exercise(
  pg::Options options,
  weave::Context &ctx
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  ,
  weave::Runtime *runtime = nullptr
#endif
)
{
  auto pool = co_await pg::Pool::create(options, {.max_connections = 2});
  check(pool.stats().idle == 1);
  {
    auto first = co_await pool.acquire();
    check(pool.stats().borrowed == 1);
    co_await first->query("BEGIN");
  }
  {
    auto second = co_await pool.acquire();
    check(second->transaction() == pg::Transaction::idle);
    auto reset = co_await weave::as_result(second->reset(options));
    check(!reset && reset.error() == std::errc::operation_not_supported);
  }

  std::vector<weave::JoinHandle<void>> jobs;
  for (unsigned index = 0; index < 32; ++index) {
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
    auto job = runtime ? runtime->spawn(borrower(pool)) : ctx.spawn(borrower(pool));
#else
    auto job = ctx.spawn(borrower(pool));
#endif
    check(job.has_value());
    jobs.push_back(std::move(*job));
  }
  for (auto &job : jobs)
    co_await std::move(job);

  check(pool.stats().created == 2 && pool.stats().idle == 2);
  check(pool.stats().borrowed == 0 && pool.stats().pending == 0);
  {
    auto first = co_await pool.acquire();
    auto second = co_await pool.acquire();
    auto timed = co_await weave::as_result(weave::timeout(10ms, pool.acquire()));
    check(!timed && timed.error() == std::errc::timed_out);
    check(pool.stats().pending == 0 && pool.stats().borrowed == 2);

#ifdef WEAVE_POSTGRES_TEST_RUNTIME
    auto waiting = runtime ? runtime->spawn(pool.acquire()) : ctx.spawn(pool.acquire());
#else
    auto waiting = ctx.spawn(pool.acquire());
#endif
    check(waiting.has_value());
    co_await weave::timeout(2s, wait_pending(pool));
    waiting->cancel();
    auto cancelled = co_await weave::as_result(std::move(*waiting));
    check(!cancelled && cancelled.error() == std::errc::operation_canceled);
    check(pool.stats().pending == 0 && pool.stats().borrowed == 2);

#ifdef WEAVE_POSTGRES_TEST_RUNTIME
    auto closing = runtime ? runtime->spawn(pool.acquire()) : ctx.spawn(pool.acquire());
#else
    auto closing = ctx.spawn(pool.acquire());
#endif
    check(closing.has_value());
    co_await weave::timeout(2s, wait_pending(pool));
    pool.close();
    auto closed = co_await weave::as_result(std::move(*closing));
    check(!closed && closed.error() == pg::make_error_code(pg::Error::closed));
    check(pool.stats().pending == 0 && pool.stats().borrowed == 2);
    co_await first->query("NOOP");
  }

  pool.close();
  auto closed = co_await weave::as_result(pool.acquire());
  check(!closed && closed.error() == pg::make_error_code(pg::Error::closed));
  check(pool.stats().idle == 0 && pool.stats().discarded == 2);

  auto expiring = co_await pg::Pool::create(options, {.max_connections = 1, .idle_timeout = 1ms});
  co_await weave::sleep_for(5ms);
  auto replacement = co_await expiring.acquire();
  check(expiring.stats().created == 2 && expiring.stats().discarded == 1);
  replacement.discard();
  check(expiring.stats().discarded == 2 && expiring.stats().idle == 0);

  std::optional<weave::Task<pg::PooledConnection>> deferred;
  {
    auto lazy = co_await pg::Pool::create(options, {.min_connections = 0});
    deferred.emplace(lazy.acquire());
  }
  auto rejected = co_await weave::as_result(std::move(*deferred));
  check(!rejected && rejected.error() == pg::make_error_code(pg::Error::closed));

  {
    auto limited = co_await pg::Pool::create(options, {.max_connections = 1, .acquire_timeout = 10ms});
    {
      auto held = co_await limited.acquire();
      auto timed = co_await weave::as_result(limited.acquire());
      check(!timed && timed.error() == std::errc::timed_out);
      check(limited.stats().pending == 0 && limited.stats().borrowed == 1);
    }
    auto available = co_await limited.acquire();
    check(limited.stats().created == 1 && limited.stats().borrowed == 1);
  }

  {
    auto lifetime = co_await pg::Pool::create(
      options,
      {.max_connections = 1, .idle_timeout = 0ms, .max_lifetime = 1ms});
    co_await weave::sleep_for(5ms);
    auto renewed = co_await lifetime.acquire();
    check(lifetime.stats().created == 2 && lifetime.stats().discarded == 1);
  }

  std::optional<pg::PooledConnection> retained;
  {
    auto owner = co_await pg::Pool::create(options, {.max_connections = 1});
    retained.emplace(co_await owner.acquire());
  }
  co_await (*retained)->query("NOOP");
  retained.reset();

  {
    auto recovery = co_await pg::Pool::create(options, {.max_connections = 1});
    {
      auto poisoned = co_await recovery.acquire();
      co_await poisoned->query("FAIL_RESET");
    }
    auto failure = co_await weave::as_result(recovery.acquire());
    check(!failure && pg::sqlstate(failure.error()) == "08006");
    check(recovery.stats().borrowed == 0 && recovery.stats().pending == 0);
    check(recovery.stats().idle == 0 && recovery.stats().discarded == 1);

    auto fresh = co_await recovery.acquire();
    check(recovery.stats().created == 2);
    co_await fresh->query("NOOP");
  }

  auto invalid = co_await weave::as_result(pg::Pool::create(options, {.max_connections = 0}));
  check(!invalid && invalid.error() == std::errc::invalid_argument);
}

int main(int argc, char **argv)
{
#if defined(_WIN32)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  check(argc == 3);
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  const std::string_view mode{argv[2]};

  if (mode == "context") {
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    auto result = ctx->run(weave::timeout(10s, exercise(options, *ctx)));
    if (!result)
      return weave::report_error(result.error());
  }
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  else {
    const bool stealing = mode.ends_with("stealing");
    const auto scheduler = stealing ? weave::Scheduler::work_stealing : weave::Scheduler::worker_affine;
    const auto layout = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
    check(runtime.has_value());

    if (stealing) {
      auto result = runtime->run([&](weave::Context &ctx) {
        return weave::timeout(10s, exercise(options, ctx, &*runtime));
      });
      if (!result)
        return weave::report_error(result.error());
    } else {
      std::vector<weave::JoinHandle<void>> jobs;
      for (std::size_t worker = 0; worker < 4; ++worker) {
        auto job = runtime->spawn_on(worker, [&](weave::Context &ctx) {
          return weave::timeout(10s, exercise(options, ctx));
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
  }
#endif

  std::printf("Pool: %s, %u checks passed\n", argv[2], checks.load());
}
