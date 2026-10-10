#include <libpq-fe.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

int main(int argc, char **argv)
{
  if (argc != 5)
    return 1;
  if (PQlibVersion() != WEAVE_POSTGRES_TEST_LIBPQ_VERSION)
    return 1;
  std::string_view mode = argv[3];
  std::string ports = argv[1];
  std::string hosts = "127.0.0.1";
  if (mode == "addresses_refused") {
    ports = argv[2];
    hosts = "localhost";
  } else if (mode == "refused") {
    ports = std::string{argv[2]} + "," + argv[2];
    hosts += ",127.0.0.1";
  } else if (mode == "fallback") {
    ports = std::string{argv[2]} + "," + argv[1];
    hosts += ",127.0.0.1";
  } else if (mode.starts_with("target") || mode == "prefer") {
    ports += "," + ports;
    hosts += ",127.0.0.1";
  }
  const std::array<const char *, 9>
    names{"host", "port", "user", "dbname", "sslmode", "gssencmode", "target_session_attrs", "password", nullptr};
  const std::array<const char *, 9> values{
    hosts.c_str(),
    ports.c_str(),
    "probe",
    "probe",
    "disable",
    "disable",
    mode == "prefer" ? "prefer-standby" : (mode.starts_with("target") ? "read-only" : "any"),
    "secret-pass-report",
    nullptr};
  auto connection = PQconnectdbParams(names.data(), values.data(), 0);
  if (!connection)
    return 1;
  bool success = mode == "success" || mode == "fallback" || mode == "prefer";
  bool matches = (PQstatus(connection) == CONNECTION_OK) == success;
  auto message = std::string{PQerrorMessage(connection)};
  if (mode == "sql" || mode == "target_sql")
    matches = matches && message.find("denied") != std::string::npos;
  if (mode == "refused") {
    auto first = message.find("connection to server");
    matches = matches && first != std::string::npos &&
      message.find("connection to server", first + 1) != std::string::npos;
  }
  matches = matches && message.find("secret-pass-report") == std::string::npos;
  PQfinish(connection);
  std::printf("{\"checks\":%u,\"libpq\":%d}\n", 1u, PQlibVersion());
  if (!matches)
    std::fprintf(stderr, "%s", message.c_str());
  return matches ? EXIT_SUCCESS : EXIT_FAILURE;
}
