#include <weave/mysql.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <cstdlib>

static weave::Task<void> query(weave::mysql::Options options)
{
  auto db = co_await weave::mysql::connect(std::move(options));
  auto result = co_await db.execute("SELECT 42 AS id");
  WEAVE_LOG_INFO("Rows over verified TLS: %zu", result.rows.size());
  co_await db.disconnect();
}

int main(int argc, char **argv)
{
  if (argc < 2 || argc > 3) {
    WEAVE_LOG_ERROR("Usage: mysql_tls <ca.pem> [port]");
    return EXIT_FAILURE;
  }
  auto port = weave::parse_port(argc == 3 ? argv[2] : "3306");
  if (!port)
    return weave::report_error(port.error());

  auto tls = weave::TlsContext::client({.ca_file = argv[1]});
  if (!tls)
    return weave::report_error(tls.error());
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(query(
    {.host = "127.0.0.1",
      .port = *port,
      .user = "weave",
      .password = std::getenv("MYSQL_PASSWORD"),
      .tls = std::move(*tls)}));
  if (!result)
    return weave::report_error(result.error());
}
