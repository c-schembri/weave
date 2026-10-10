#include <libpq-fe.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void hex(const char *value)
{
  for (auto pointer = reinterpret_cast<const unsigned char *>(value); *pointer; ++pointer)
    std::printf("%02x", static_cast<unsigned>(*pointer));
}

static void optional(const char *value)
{
  if (!value) {
    std::printf("null");
    return;
  }
  std::printf("\"");
  hex(value);
  std::printf("\"");
}

static PGconn *connect(const char *port, const char *options)
{
  const char *keys[]{
    "host",
    "port",
    "user",
    "dbname",
    "sslmode",
    "gssencmode",
    "connect_timeout",
    "target_session_attrs",
    "options",
    nullptr};
  const char *values[]{"127.0.0.1", port, "test", "test", "disable", "disable", "3", "any", options, nullptr};
  auto connection = PQconnectdbParams(keys, values, 0);
  if (!connection || PQstatus(connection) != CONNECTION_OK) {
    std::fprintf(stderr, "%s", connection ? PQerrorMessage(connection) : "Allocation failure\n");
    PQfinish(connection);
    std::exit(1);
  }
  return connection;
}

static void snapshot(PGconn *connection, unsigned phase)
{
  std::printf("{\"phase\":%u,\"version\":%d,\"raw\":", phase, PQserverVersion(connection));
  optional(PQparameterStatus(connection, "server_version"));
  std::printf(",\"options\":\"");
  hex(PQoptions(connection));
  std::printf("\",\"empty\":");
  optional(PQparameterStatus(connection, "weave.fixture.empty"));
  std::printf(",\"value\":");
  optional(PQparameterStatus(connection, "weave.fixture.value"));
  std::printf(",\"absent\":");
  optional(PQparameterStatus(connection, "weave.fixture.missing"));
  std::printf("}\n");
}

int main(int argc, char **argv)
{
  if (argc != 3)
    return 1;
  if (PQlibVersion() != WEAVE_POSTGRES_TEST_LIBPQ_VERSION) {
    std::fprintf(stderr, "Loaded libpq does not match configured headers\n");
    return 1;
  }
  std::printf("libpq: %d\n", PQlibVersion());
  auto connection = connect(argv[1], "-c metadata.fixture=initial");
  snapshot(connection, 0);
  auto result = PQexec(connection, "NOOP");
  if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
    std::fprintf(stderr, "%s", PQerrorMessage(connection));
    PQclear(result);
    PQfinish(connection);
    return 1;
  }
  PQclear(result);
  snapshot(connection, 1);
  PQfinish(connection);
  connection = connect(argv[1], "-c metadata.fixture=reset");
  snapshot(connection, 2);
  PQfinish(connection);
}
