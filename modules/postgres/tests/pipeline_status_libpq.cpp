#include <libpq-fe.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <source_location>
#include <string_view>

static unsigned checks = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native pipeline status check failed: %u\n", where.line());
    std::_Exit(1);
  }
}

static void state(
  PGconn *connection,
  PGpipelineStatus expected,
  std::source_location where = std::source_location::current())
{
  check(PQpipelineStatus(connection) == expected, where);
}

static void send(PGconn *connection, const char *sql)
{
  check(PQsendQueryParams(connection, sql, 0, nullptr, nullptr, nullptr, nullptr, 0) == 1);
}

static void flush(PGconn *connection)
{
  check(PQsendFlushRequest(connection) == 1 && PQflush(connection) == 0);
}

static void result(PGconn *connection, ExecStatusType expected, PGpipelineStatus pipeline)
{
  auto value = PQgetResult(connection);
  check(value && PQresultStatus(value) == expected);
  state(connection, pipeline);
  PQclear(value);
  check(!PQgetResult(connection));
}

int main(int argc, char **argv)
{
  check(argc == 4 && PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  const char *keys[] = {"host", "port", "user", "dbname", "sslmode", "gssencmode", nullptr};
  const char *values[] = {"127.0.0.1", argv[1], "test", "test", "disable", "disable", nullptr};
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{PQconnectdbParams(keys, values, 0), PQfinish};
  check(connection && PQstatus(connection.get()) == CONNECTION_OK);
  PQsetNoticeProcessor(
    connection.get(),
    [](void *, const char *) {
    },
    nullptr);
  state(nullptr, PQ_PIPELINE_OFF);
  state(connection.get(), PQ_PIPELINE_OFF);

  check(PQenterPipelineMode(connection.get()));
  state(connection.get(), PQ_PIPELINE_ON);
  check(PQexitPipelineMode(connection.get()));
  state(connection.get(), PQ_PIPELINE_OFF);

  check(PQenterPipelineMode(connection.get()));
  send(connection.get(), "ERROR");
  send(connection.get(), "NOOP");
  flush(connection.get());
  result(connection.get(), PGRES_FATAL_ERROR, PQ_PIPELINE_ABORTED);
  result(connection.get(), PGRES_PIPELINE_ABORTED, PQ_PIPELINE_ABORTED);
  state(connection.get(), PQ_PIPELINE_ABORTED);
  auto unsynchronized_exit = PQexitPipelineMode(connection.get());
  check(unsynchronized_exit == 1);
  state(connection.get(), PQ_PIPELINE_OFF);
  std::printf("Native consumed-unsynchronized exit: %d\n", unsynchronized_exit);
  check(PQenterPipelineMode(connection.get()));
  state(connection.get(), PQ_PIPELINE_ON);
  check(PQsendPipelineSync(connection.get()));
  state(connection.get(), PQ_PIPELINE_ON);
  flush(connection.get());
  result(connection.get(), PGRES_PIPELINE_SYNC, PQ_PIPELINE_ON);
  check(PQexitPipelineMode(connection.get()));
  state(connection.get(), PQ_PIPELINE_OFF);

  check(PQenterPipelineMode(connection.get()));
  send(connection.get(), "BEGIN");
  send(connection.get(), "ERROR");
  send(connection.get(), "NOOP");
  check(PQsendPipelineSync(connection.get()));
  flush(connection.get());
  result(connection.get(), PGRES_COMMAND_OK, PQ_PIPELINE_ON);
  result(connection.get(), PGRES_FATAL_ERROR, PQ_PIPELINE_ABORTED);
  result(connection.get(), PGRES_PIPELINE_ABORTED, PQ_PIPELINE_ABORTED);
  result(connection.get(), PGRES_PIPELINE_SYNC, PQ_PIPELINE_ON);
  check(PQtransactionStatus(connection.get()) == PQTRANS_INERROR);
  check(PQexitPipelineMode(connection.get()));
  state(connection.get(), PQ_PIPELINE_OFF);
  auto rollback = PQexec(connection.get(), "ROLLBACK");
  check(rollback && PQresultStatus(rollback) == PGRES_COMMAND_OK);
  PQclear(rollback);
  check(PQtransactionStatus(connection.get()) == PQTRANS_IDLE);

  if (std::string_view{argv[3]} != "normal") {
    check(PQenterPipelineMode(connection.get()));
    send(connection.get(), std::string_view{argv[3]} == "bad" ? "BAD" : "EOF");
    flush(connection.get());
    auto failed = PQgetResult(connection.get());
    check(failed && PQresultStatus(failed) == PGRES_FATAL_ERROR);
    PQclear(failed);
    auto health = PQstatus(connection.get());
    std::printf("Native terminal connection status: %d\n", static_cast<int>(health));
    check(health == (std::string_view{argv[3]} == "bad" ? CONNECTION_OK : CONNECTION_BAD));
    check(PQpipelineStatus(connection.get()) != PQ_PIPELINE_OFF);
    std::printf("Native terminal pipeline status: %d\n", static_cast<int>(PQpipelineStatus(connection.get())));
  }

  std::printf("Native pipeline status controls passed: %u checks; libpq: %d\n", checks, PQlibVersion());
}
