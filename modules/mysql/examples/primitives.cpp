#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <cstdlib>
#include <optional>
#include <string_view>

static weave::Task<void> query(weave::u16 port)
{
  auto db = co_await weave::mysql::connect({.port = port, .user = "weave", .password = std::getenv("MYSQL_PASSWORD")});
  auto result = co_await db.execute(
    "SELECT 42 AS id, 12.5e0 AS weight, TRUE AS delivered, 'Alex' AS recipient, NULL AS notes");

  for (const auto &row : result.rows) {
    auto id = row["id"].as<weave::i64>();
    auto weight = row["weight"].as<double>();
    auto delivered = row["delivered"].as<bool>();
    auto recipient = row["recipient"].as<std::string_view>();
    auto notes = row["notes"].as<std::optional<std::string_view>>();

    if (!id)
      co_await weave::fail(id.error());
    if (!weight)
      co_await weave::fail(weight.error());
    if (!delivered)
      co_await weave::fail(delivered.error());
    if (!recipient)
      co_await weave::fail(recipient.error());
    if (!notes)
      co_await weave::fail(notes.error());

    WEAVE_LOG_INFO("%lld: %.1f kg, delivered=%d", static_cast<long long>(*id), *weight, *delivered);
    WEAVE_LOG_INFO(
      "Recipient: %.*s, notes present=%d",
      static_cast<int>(recipient->size()),
      recipient->data(),
      notes->has_value());
  }

  co_await db.disconnect();
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "3306");
  if (argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: mysql_primitives [port]");
    return EXIT_FAILURE;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(query(*port));
  if (!result)
    return weave::report_error(result.error());
}
