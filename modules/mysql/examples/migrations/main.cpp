#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include "mysql_migrations.hpp"
#include <cstdlib>

static weave::Task<void> migrate(weave::u16 port)
{
  auto db = co_await weave::mysql::connect(
    {.port = port, .user = "weave", .password = std::getenv("MYSQL_PASSWORD"), .database = "shipping"});
  const auto manifest = mysql_migrations();
  auto report = co_await weave::db::migrate(db, manifest);
  WEAVE_LOG_INFO("Applied: %zu", report.applied);
  co_await db.disconnect();
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "3306");
  if (argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: mysql_migrations [port]");
    return EXIT_FAILURE;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(migrate(*port));
  if (!result)
    return weave::report_error(result.error());
}
