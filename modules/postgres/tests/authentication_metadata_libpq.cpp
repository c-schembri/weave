#include <libpq-fe.h>
#include <cstdio>
#include <cstdlib>
#include <string_view>

int main(int argc, char **argv)
{
  if (argc != 5)
    return 1;
  if (PQlibVersion() != WEAVE_POSTGRES_TEST_LIBPQ_VERSION)
    return 1;
  std::string_view scenario = argv[3];
  auto password = scenario.ends_with("missing") || scenario == "scram_keys" || scenario == "reset_keys"
    ? ""
    : "secret-source";
  bool keys = scenario == "scram_keys" || scenario == "reset_keys";
  const char *names[] = {
    "host",
    "port",
    "user",
    "dbname",
    "password",
    "sslmode",
    "gssencmode",
    "connect_timeout",
    "channel_binding",
    "passfile",
    "scram_client_key",
    "scram_server_key",
    "application_name",
    nullptr};
  const char *values[] = {
    "127.0.0.1",
    argv[1],
    "test",
    "test",
    password,
    "disable",
    "disable",
    "3",
    "disable",
    argv[4],
    keys ? "Tcn749j882CnenZTl3ZR7IGG65V2ZkUuWRy7HLaHwm0=" : nullptr,
    keys ? "cKMH7kFpT91Ant4x4cVouJ9kma3Q5QACQagodKK123Y=" : nullptr,
    "auth-info-initial",
    nullptr};
  auto *connection = PQconnectdbParams(names, values, 0);
  if (!connection)
    return 1;
  if (scenario.starts_with("reset")) {
    if (PQstatus(connection) != CONNECTION_OK || PQconnectionUsedPassword(connection))
      return 1;
    PQreset(connection);
  }
  std::printf("libpq: %d\n", PQlibVersion());
  std::printf(
    "{\"success\":%s,\"requested\":%s,\"missing\":%s}\n",
    PQstatus(connection) == CONNECTION_OK ? "true" : "false",
    PQconnectionUsedPassword(connection) ? "true" : "false",
    PQconnectionNeedsPassword(connection) ? "true" : "false");
  PQfinish(connection);
}
