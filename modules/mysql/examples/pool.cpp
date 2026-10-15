#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <cstdlib>

static weave::Task<void> query(weave::u16 port)
{
  auto pool = co_await weave::mysql::Pool::create(
    {.port = port, .user = "weave", .password = std::getenv("MYSQL_PASSWORD")},
    {.max_connections = 8});
  auto db = co_await pool.acquire();
  auto result = co_await db->execute("SELECT 42 AS id");

  for (const auto &row : result.rows) {
    auto id = row["id"].as<int>();
    if (!id)
      co_await weave::fail(id.error());
    WEAVE_LOG_INFO("ID: %d", *id);
  }
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "3306");
  if (argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: mysql_pool [port]");
    return EXIT_FAILURE;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(query(*port));
  if (!result)
    return weave::report_error(result.error());
}
