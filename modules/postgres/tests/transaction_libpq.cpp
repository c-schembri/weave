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
    std::fprintf(stderr, "Native transaction check failed: %u\n", where.line());
    std::_Exit(1);
  }
}

static void state(
  PGconn *connection,
  PGTransactionStatusType expected,
  std::source_location where = std::source_location::current())
{
  auto actual = PQtransactionStatus(connection);
  if (actual != expected)
    std::fprintf(stderr, "Native transaction actual=%d expected=%d at %u\n", actual, expected, where.line());
  check(actual == expected, where);
}

static PGconn *connect(const char *port)
{
  const char *keys[]{"host", "port", "user", "dbname", "sslmode", "gssencmode", "connect_timeout", nullptr};
  const char *values[]{"127.0.0.1", port, "test", "test", "disable", "disable", "5", nullptr};
  auto connection = PQconnectdbParams(keys, values, 0);
  check(connection && PQstatus(connection) == CONNECTION_OK);
  PQsetNoticeProcessor(
    connection,
    [](void *, const char *) {
    },
    nullptr);
  return connection;
}

static void query(PGconn *connection, const char *sql, PGTransactionStatusType expected, bool error = false)
{
  check(PQsendQuery(connection, sql));
  state(connection, PQTRANS_ACTIVE);
  unsigned results = 0;
  for (;;) {
    auto result = PQgetResult(connection);
    if (!result)
      break;
    check(PQresultStatus(result) == (error ? PGRES_FATAL_ERROR : PGRES_COMMAND_OK));
    PQclear(result);
    ++results;
  }
  check(results == 1);
  state(connection, expected);
}

int main(int argc, char **argv)
{
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  if (argc != 4)
    return 1;
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{connect(argv[1]), PQfinish};
  state(connection.get(), PQTRANS_IDLE);
  state(nullptr, PQTRANS_UNKNOWN);
  query(connection.get(), "BEGIN", PQTRANS_INTRANS);
  query(connection.get(), "ERROR", PQTRANS_INERROR, true);
  query(connection.get(), "ROLLBACK", PQTRANS_IDLE);
  query(connection.get(), "NOTICE", PQTRANS_IDLE);
  query(connection.get(), "NOOP", PQTRANS_IDLE);

  check(PQsendQuery(connection.get(), "ROWS") && PQsetSingleRowMode(connection.get()));
  auto row = PQgetResult(connection.get());
  check(row && PQresultStatus(row) == PGRES_SINGLE_TUPLE);
  PQclear(row);
  state(connection.get(), PQTRANS_ACTIVE);
  auto complete = PQgetResult(connection.get());
  check(complete && PQresultStatus(complete) == PGRES_TUPLES_OK);
  PQclear(complete);
  check(!PQgetResult(connection.get()));
  state(connection.get(), PQTRANS_IDLE);

  auto copy = PQexec(connection.get(), "COPY");
  check(copy && PQresultStatus(copy) == PGRES_COPY_OUT);
  PQclear(copy);
  state(connection.get(), PQTRANS_ACTIVE);
  char *data = nullptr;
  check(PQgetCopyData(connection.get(), &data, 0) > 0);
  PQfreemem(data);
  state(connection.get(), PQTRANS_ACTIVE);
  check(PQgetCopyData(connection.get(), &data, 0) == -1);
  auto copied = PQgetResult(connection.get());
  check(copied && PQresultStatus(copied) == PGRES_COMMAND_OK);
  PQclear(copied);
  check(!PQgetResult(connection.get()));
  state(connection.get(), PQTRANS_IDLE);
  query(connection.get(), "EXCHANGE", PQTRANS_IDLE);

  check(PQenterPipelineMode(connection.get()));
  check(PQsendQueryParams(connection.get(), "NOOP", 0, nullptr, nullptr, nullptr, nullptr, 0));
  state(connection.get(), PQTRANS_ACTIVE);
  check(PQsendFlushRequest(connection.get()) && PQflush(connection.get()) == 0);
  auto command = PQgetResult(connection.get());
  check(command && PQresultStatus(command) == PGRES_COMMAND_OK);
  PQclear(command);
  check(!PQgetResult(connection.get()));
  state(connection.get(), PQTRANS_IDLE);
  std::printf("Native consumed-unsynchronized status: %d\n", static_cast<int>(PQtransactionStatus(connection.get())));
  check(PQpipelineSync(connection.get()));
  auto sync = PQgetResult(connection.get());
  check(sync && PQresultStatus(sync) == PGRES_PIPELINE_SYNC);
  PQclear(sync);
  std::printf("Native post-Sync buffered status: %d\n", static_cast<int>(PQtransactionStatus(connection.get())));
  check(!PQgetResult(connection.get()));
  check(PQexitPipelineMode(connection.get()));
  state(connection.get(), PQTRANS_IDLE);

  if (std::string_view{argv[3]} == "normal") {
    connection.reset();
    connection.reset(connect(argv[1]));
    state(connection.get(), PQTRANS_IDLE);
  } else {
    auto failed = PQexec(connection.get(), "EOF");
    if (failed)
      PQclear(failed);
    check(PQstatus(connection.get()) == CONNECTION_BAD);
    state(connection.get(), PQTRANS_UNKNOWN);
  }
  std::printf("Native transaction controls passed: %u checks; libpq: %d\n", checks, PQlibVersion());
}
