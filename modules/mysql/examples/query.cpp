#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <cstdlib>
#include <string>
#include <utility>

struct Shipment {
  weave::i64 id;
  std::string recipient;
};

static weave::Task<void> example(weave::mysql::Options options)
{
  auto db = co_await weave::mysql::connect(std::move(options));
  auto result = co_await db.execute("SELECT 42 AS id, 'Ada' AS recipient");

  for (const auto &row : result.rows) {
    auto shipment = row.as<Shipment>();
    if (!shipment)
      co_await weave::fail(shipment.error());

    WEAVE_LOG_INFO("Shipment %lld for %s", static_cast<long long>(shipment->id), shipment->recipient.c_str());
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

  weave::mysql::Options options{.port = *port, .user = "weave", .password = std::getenv("MYSQL_PASSWORD")};
  auto result = ctx->run(example(std::move(options)));
  if (!result)
    return weave::report_error(result.error());
}
