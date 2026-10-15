#include <weave/mysql.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <cstdlib>

int main(int argc, char **argv)
{
  auto port = weave::parse_port(argc == 2 ? argv[1] : "3306");
  if (argc > 2 || !port) {
    WEAVE_LOG_ERROR("Usage: mysql_blocking [port]");
    return EXIT_FAILURE;
  }

  auto db = weave::mysql::BlockingConnection::connect(
    {.port = *port, .user = "weave", .password = std::getenv("MYSQL_PASSWORD")});
  if (!db)
    return weave::report_error(db.error());

  auto result = db->execute("SELECT 42 AS id");
  if (!result)
    return weave::report_error(result.error());

  for (const auto &row : result->rows) {
    auto id = row["id"].as<int>();
    if (!id)
      return weave::report_error(id.error());
    WEAVE_LOG_INFO("ID: %d", *id);
  }

  if (auto closed = db->disconnect(); !closed)
    return weave::report_error(closed.error());
}
