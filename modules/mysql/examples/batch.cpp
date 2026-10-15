#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <cstdlib>
#include <vector>

static weave::Task<void> query(weave::u16 port)
{
  auto db = co_await weave::mysql::connect({.port = port, .user = "weave", .password = std::getenv("MYSQL_PASSWORD")});
  std::vector<weave::mysql::Command> commands{
    {.sql = "SELECT 1 AS id"},
    {.sql = "SELECT * FROM information_schema.weave_example_missing_table"},
    {.sql = "SELECT ? AS id", .parameters = {42}}};
  auto outcomes = co_await db.batch(std::move(commands));

  // A completed SQL error is an outcome; later commands still run. This is not a transaction.
  for (const auto &outcome : outcomes) {
    if (outcome.failure) {
      WEAVE_LOG_ERROR("SQLSTATE %s: %s", outcome.failure->sqlstate.c_str(), outcome.failure->message.c_str());
    } else {
      WEAVE_LOG_INFO("Results: %zu", outcome.results.size());
    }
  }

  co_await db.disconnect();
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "3306");
  if (argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: mysql_batch [port]");
    return EXIT_FAILURE;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(query(*port));
  if (!result)
    return weave::report_error(result.error());
}
