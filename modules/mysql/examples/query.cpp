#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <cstdlib>

struct Shipment {
  weave::i64 id;
  std::string name;
};

static weave::Task<void> query(weave::u16 port)
{
  const char *password = std::getenv("MYSQL_PASSWORD");
  auto db = co_await weave::mysql::connect({.port = port, .user = "weave", .password = password});
  auto result = co_await db.execute("SELECT 42 AS id, 'shipment' AS name");

  for (const auto &row : result.rows) {
    auto shipment = row.as<Shipment>();
    if (!shipment)
      co_await weave::fail(shipment.error());
    WEAVE_LOG_INFO("%lld: %s", static_cast<long long>(shipment->id), shipment->name.c_str());
  }

  co_await db.disconnect();
}

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "3306");
  if (argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: mysql_query [port]");
    return EXIT_FAILURE;
  }

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(query(*port));
  if (!result)
    return weave::report_error(result.error());
}
