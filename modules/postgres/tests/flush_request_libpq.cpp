#include <libpq-fe.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>

static unsigned checks = 0;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native Flush request check failed: %u\n", location.line());
    std::exit(1);
  }
}

static void result(PGconn *connection, ExecStatusType expected)
{
  auto value = PQgetResult(connection);
  check(value && PQresultStatus(value) == expected);
  PQclear(value);
  check(!PQgetResult(connection));
}

static void execute(PGconn *connection, const char *sql)
{
  check(PQsendQueryParams(connection, sql, 0, nullptr, nullptr, nullptr, nullptr, 0) == 1);
}

int main(int argc, char **argv)
{
  check(argc == 3 && PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  const char *keys[] = {"host", "port", "user", "dbname", "sslmode", "gssencmode", nullptr};
  const char *values[] = {"127.0.0.1", argv[1], "test", "test", "disable", "disable", nullptr};
  std::unique_ptr<PGconn, decltype(&PQfinish)> owner{PQconnectdbParams(keys, values, 0), PQfinish};
  auto connection = owner.get();
  check(connection && PQstatus(connection) == CONNECTION_OK);

  check(PQenterPipelineMode(connection) == 1);
  check(PQsendFlushRequest(connection) == 1 && PQflush(connection) == 0);
  check(!PQgetResult(connection));
  check(PQpipelineStatus(connection) == PQ_PIPELINE_ON && PQtransactionStatus(connection) == PQTRANS_IDLE);
  check(PQexitPipelineMode(connection) == 1);

  check(PQenterPipelineMode(connection) == 1);
  execute(connection, "NOOP");
  check(PQsendFlushRequest(connection) == 1);
  execute(connection, "NOOP");
  check(PQsendPipelineSync(connection) == 1);
  check(PQsendFlushRequest(connection) == 1 && PQflush(connection) == 0);
  result(connection, PGRES_COMMAND_OK);
  result(connection, PGRES_COMMAND_OK);
  result(connection, PGRES_PIPELINE_SYNC);
  check(PQpipelineStatus(connection) == PQ_PIPELINE_ON);
  check(PQexitPipelineMode(connection) == 1);

  check(PQenterPipelineMode(connection) == 1);
  execute(connection, "ERROR");
  check(PQsendFlushRequest(connection) == 1);
  execute(connection, "NOOP");
  check(PQsendFlushRequest(connection) == 1 && PQflush(connection) == 0);
  result(connection, PGRES_FATAL_ERROR);
  result(connection, PGRES_PIPELINE_ABORTED);
  check(PQpipelineStatus(connection) == PQ_PIPELINE_ABORTED);
  check(PQsendFlushRequest(connection) == 1 && PQflush(connection) == 0);
  check(PQpipelineStatus(connection) == PQ_PIPELINE_ABORTED);
  check(PQsendPipelineSync(connection) == 1);
  check(PQsendFlushRequest(connection) == 1 && PQflush(connection) == 0);
  result(connection, PGRES_PIPELINE_SYNC);
  check(PQpipelineStatus(connection) == PQ_PIPELINE_ON && PQtransactionStatus(connection) == PQTRANS_IDLE);
  check(PQexitPipelineMode(connection) == 1);
  owner.reset();
  std::printf("Native queueable Flush controls passed: %u checks; libpq: %d\n", checks, PQlibVersion());
}
