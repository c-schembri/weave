#include <weave/postgres.hpp>
#include <weave/log.hpp>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace pg = weave::pg;

static weave::Task<void> query(pg::Pool &pool)
{
  auto db = co_await pool.acquire();
  auto result = co_await db->execute_borrowed("SELECT pg_backend_pid() AS session");

  for (const auto &row : result.rows) {
    auto session = row["session"].as<int>();
    if (!session)
      co_await weave::fail(session.error());

    std::printf("Session %d\n", *session);
  }
  // The lease returns to the pool at scope exit; do not finish the connection.
}

static weave::Task<void> example(pg::Options options)
{
  auto pool = co_await pg::Pool::create(std::move(options), {.max_connections = 2, .min_connections = 2});

  // Four concurrent requests share two sessions on the calling Context.
  co_await weave::when_all(query(pool), query(pool), query(pool), query(pool));

  auto stats = pool.stats();
  std::printf("Pool: %zu idle, %zu borrowed\n", stats.idle, stats.borrowed);
}

int main(int argc, char **argv)
{
  if (argc > 2) {
    WEAVE_LOG_ERROR("Usage: postgres_pool [connection-string]");
    return EXIT_FAILURE;
  }

  auto options = pg::Options::load(argc == 2 ? argv[1] : "");
  if (!options)
    return weave::report_error(options.error());

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(example(std::move(*options)));
  if (!result)
    return weave::report_error(result.error());
}
