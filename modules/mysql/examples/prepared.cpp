#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <array>
#include <cstdlib>

static weave::Task<void> query(weave::u16 port)
{
  auto db = co_await weave::mysql::connect({.port = port, .user = "weave", .password = std::getenv("MYSQL_PASSWORD")});
  auto statement = co_await db.prepare("SELECT CAST(? AS SIGNED) AS id, CAST(? AS CHAR) AS recipient");
  const std::array<weave::mysql::ParameterView, 2> parameters{42, "Alex"};
  auto result = co_await statement.execute_borrowed(parameters);

  for (const auto &row : result.rows) {
    auto recipient = row["recipient"].as<std::string_view>();
    if (!recipient)
      co_await weave::fail(recipient.error());
    WEAVE_LOG_INFO("Recipient: %.*s", static_cast<int>(recipient->size()), recipient->data());
  }

  co_await statement.close();
  co_await db.disconnect();
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "3306");
  if (argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: mysql_prepared [port]");
    return EXIT_FAILURE;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(query(*port));
  if (!result)
    return weave::report_error(result.error());
}
