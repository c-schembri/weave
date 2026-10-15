#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <cstdlib>

static weave::Task<void> query(weave::u16 port)
{
  auto db = co_await weave::mysql::connect({.port = port, .user = "weave", .password = std::getenv("MYSQL_PASSWORD")});
  auto stream = co_await db.stream("SELECT 1 AS id UNION ALL SELECT 2");

  while (auto row = co_await stream.next()) {
    auto id = (*row)["id"].as<int>();
    if (!id)
      co_await weave::fail(id.error());
    WEAVE_LOG_INFO("ID: %d", *id);
  }

  co_await stream.finish();
  co_await db.disconnect();
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "3306");
  if (argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: mysql_stream [port]");
    return EXIT_FAILURE;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(query(*port));
  if (!result)
    return weave::report_error(result.error());
}
