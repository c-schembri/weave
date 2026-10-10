#include <libpq-fe.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <source_location>
#include <string>

static unsigned checks = 0;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native exchange chunks check failed: %u\n", location.line());
    std::exit(1);
  }
}

static void query(
  PGconn *connection,
  const char *sql,
  int size,
  unsigned expected,
  bool error = false,
  bool copy = false)
{
  check(PQsendQuery(connection, sql) == 1);
  if (size)
    check(PQsetChunkedRowsMode(connection, size) == 1);
  unsigned next = 1;
  unsigned chunks = 0;
  unsigned terminals = 0;
  bool failed = false;
  while (auto result = PQgetResult(connection)) {
    auto status = PQresultStatus(result);
    if (status == PGRES_TUPLES_CHUNK || status == PGRES_TUPLES_OK) {
      check(PQnfields(result) == 3);
      check(std::strcmp(PQfname(result, 0), "n") == 0 && PQftype(result, 0) == 23);
      check(std::strcmp(PQfname(result, 1), "payload") == 0 && PQftype(result, 1) == 25);
      check(std::strcmp(PQfname(result, 2), "nullable") == 0 && PQftype(result, 2) == 25);
      auto rows = PQntuples(result);
      if (status == PGRES_TUPLES_CHUNK) {
        check(size && rows > 0 && rows <= size);
        ++chunks;
      } else {
        check(!size || rows == 0);
        ++terminals;
      }
      for (int row = 0; row < rows; ++row) {
        check(std::string(PQgetvalue(result, row, 0)) == std::to_string(next));
        check(PQgetlength(result, row, 1) == 120);
        check(PQgetisnull(result, row, 2) == (next % 2 == 0));
        if (!PQgetisnull(result, row, 2))
          check(PQgetlength(result, row, 2) == 0);
        ++next;
      }
      if (std::strcmp(sql, "EARLY") == 0 && next == 3) {
        std::puts("Chunk prefix observed");
        std::fflush(stdout);
      }
    } else if (status == PGRES_FATAL_ERROR) {
      check(error && std::strcmp(PQresultErrorField(result, PG_DIAG_SQLSTATE), "22012") == 0);
      failed = true;
    } else if (status == PGRES_COPY_OUT) {
      check(copy);
      std::string bytes;
      char *buffer = nullptr;
      int count = 0;
      while ((count = PQgetCopyData(connection, &buffer, 0)) > 0) {
        bytes.append(buffer, count);
        PQfreemem(buffer);
      }
      check(count == -1 && bytes == "hi\n");
    } else {
      check(status == PGRES_COMMAND_OK || status == PGRES_EMPTY_QUERY);
    }
    PQclear(result);
  }
  check(next == expected + 1 && failed == error);
  if (error)
    check(chunks == 1 && terminals == 0);
  if (std::strcmp(sql, "FIVE") == 0)
    check(terminals == 1 && chunks == (size ? (5 + size - 1) / size : 0));
  check(PQstatus(connection) == CONNECTION_OK && PQtransactionStatus(connection) == PQTRANS_IDLE);
}

static void special(PGconn *connection, const char *sql, bool binary)
{
  check(PQsendQuery(connection, sql) == 1 && PQsetChunkedRowsMode(connection, 2) == 1);
  unsigned next = 1;
  unsigned chunks = 0;
  unsigned terminals = 0;
  while (auto result = PQgetResult(connection)) {
    auto status = PQresultStatus(result);
    check(status == PGRES_TUPLES_CHUNK || status == PGRES_TUPLES_OK);
    auto rows = PQntuples(result);
    if (status == PGRES_TUPLES_CHUNK) {
      check(rows > 0 && rows <= 2);
      ++chunks;
    } else {
      check(rows == 0);
      ++terminals;
    }
    check(PQnfields(result) == (binary ? 3 : 0));
    if (binary) {
      check(std::strcmp(PQfname(result, 0), "n") == 0 && PQftype(result, 0) == 23 && PQfsize(result, 0) == 4);
      check(PQftable(result, 0) == 411 && PQftablecol(result, 0) == 2 && PQfmod(result, 0) == -1);
      check(PQfformat(result, 0) == 1);
      check(std::strcmp(PQfname(result, 1), "payload") == 0 && PQftype(result, 1) == 17 && PQfsize(result, 1) == -1);
      check(PQftable(result, 1) == 411 && PQftablecol(result, 1) == 3 && PQfmod(result, 1) == -1);
      check(PQfformat(result, 1) == 1);
      check(
        std::strcmp(PQfname(result, 2), "caf\xc3\xa9") == 0 && PQftype(result, 2) == 25 && PQfsize(result, 2) == -1);
      check(PQftable(result, 2) == 0 && PQftablecol(result, 2) == 0 && PQfmod(result, 2) == 7);
      check(PQfformat(result, 2) == 0);
    }
    for (int row = 0; row < rows; ++row) {
      if (binary) {
        const std::array number{char{0}, char{0}, char{0}, static_cast<char>(next)};
        check(PQgetlength(result, row, 0) == 4 && std::memcmp(PQgetvalue(result, row, 0), number.data(), 4) == 0);
        check(PQgetlength(result, row, 1) == 4 && std::memcmp(PQgetvalue(result, row, 1), "a\0b\xff", 4) == 0);
        check(PQgetisnull(result, row, 2) == (next % 2 == 0));
        if (!PQgetisnull(result, row, 2))
          check(PQgetlength(result, row, 2) == 5 && std::memcmp(PQgetvalue(result, row, 2), "caf\xc3\xa9", 5) == 0);
      }
      ++next;
    }
    PQclear(result);
  }
  check(next == 6 && chunks == 3 && terminals == 1);
  check(PQstatus(connection) == CONNECTION_OK && PQtransactionStatus(connection) == PQTRANS_IDLE);
}

int main(int argc, char **argv)
{
  check(argc == 3 && PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  const char *keys[] = {"host", "port", "user", "dbname", "sslmode", "gssencmode", nullptr};
  const char *values[] = {"127.0.0.1", argv[1], "test", "test", "disable", "disable", nullptr};
  std::unique_ptr<PGconn, decltype(&PQfinish)> owner{PQconnectdbParams(keys, values, 0), PQfinish};
  check(owner && PQstatus(owner.get()) == CONNECTION_OK);
  if (std::strcmp(argv[2], "native:early") == 0) {
    query(owner.get(), "EARLY", 2, 5);
  } else {
    constexpr std::array sizes{0, 1, 2, 3, 5, 7};
    for (auto size : sizes)
      query(owner.get(), "FIVE", size, 5);
    query(owner.get(), "ZERO", 2, 0);
    query(owner.get(), "MULTI", 2, 5);
    query(owner.get(), "ERROR", 2, 2, true);
    query(owner.get(), "", 1, 0);
    query(owner.get(), "COPY", 2, 5, false, true);
    special(owner.get(), "NO_COLUMNS", false);
    special(owner.get(), "BINARY", true);
  }
  owner.reset();
  std::printf("Native exchange chunks controls passed: %u checks; libpq: %d\n", checks, PQlibVersion());
}
