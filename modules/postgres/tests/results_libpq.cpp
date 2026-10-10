#include <libpq-fe.h>
#include "tls_certificates.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
static unsigned checks = 0;

static void check(bool condition)
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "libpq result check failed: %u\n", checks);
    std::exit(1);
  }
}

static Result query(PGconn *connection, const char *sql, ExecStatusType status)
{
  Result result{PQexec(connection, sql), &PQclear};
  check(result && PQresultStatus(result.get()) == status);
  return result;
}

static void failure(const PGresult *result)
{
  check(result && PQresultStatus(result) == PGRES_FATAL_ERROR);
  check(std::string_view{PQresultErrorField(result, PG_DIAG_SQLSTATE)} == "22012");
  check(std::string_view{PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY)} == "division by zero");
}

int main()
{
  static fixture::Certificates certificates;
#if !defined(_WIN32)
  std::error_code error;
  std::filesystem::permissions(
    certificates.client_key,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace,
    error);
  check(!error);
#endif
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::string port, password, standby, address, local;
  std::getline(std::cin, port);
  std::getline(std::cin, password);
  std::getline(std::cin, standby);
  std::getline(std::cin, address);
  std::getline(std::cin, local);
  std::printf("Owned certificate fixture: %s\n", certificates.directory.string().c_str());
  std::printf("libpq version: %d\n", PQlibVersion());
  check(PQlibVersion() >= 180000 && PQlibVersion() < 190000);
  const std::array<const char *, 14> keys{
    "host",
    "hostaddr",
    "port",
    "user",
    "dbname",
    "password",
    "sslmode",
    "sslrootcert",
    "sslcert",
    "sslkey",
    "channel_binding",
    "gssencmode",
    "connect_timeout",
    nullptr};
  const std::array modes{"disable", "verify-full"};
  for (auto mode : modes) {
    const std::array<const char *, 14> values{
      "localhost",
      address.c_str(),
      port.c_str(),
      "weave",
      "postgres",
      password.c_str(),
      mode,
      certificates.ca.c_str(),
      certificates.client.c_str(),
      certificates.client_key.c_str(),
      "prefer",
      "disable",
      "10",
      nullptr};
    Connection connection{PQconnectdbParams(keys.data(), values.data(), 0), &PQfinish};
    check(connection && PQstatus(connection.get()) == CONNECTION_OK);
    check(
      PQsendQuery(
        connection.get(),
        "SET application_name TO 'outcomes'; SELECT 1 WHERE false; SELECT FROM generate_series(1,2); SELECT 1/0; "
        "SELECT 99") == 1);
    std::vector<Result> results;
    while (auto result = PQgetResult(connection.get()))
      results.emplace_back(result, &PQclear);
    check(results.size() == 4);
    check(PQresultStatus(results[0].get()) == PGRES_COMMAND_OK);
    check(PQresultStatus(results[1].get()) == PGRES_TUPLES_OK && PQntuples(results[1].get()) == 0);
    check(PQnfields(results[1].get()) == 1);
    check(PQresultStatus(results[2].get()) == PGRES_TUPLES_OK && PQnfields(results[2].get()) == 0);
    check(PQntuples(results[2].get()) == 2);
    failure(results[3].get());
    query(connection.get(), " ; ", PGRES_EMPTY_QUERY);
    failure(results[3].get());
    Result extended{PQexecParams(connection.get(), "SELECT 1/0", 0, nullptr, nullptr, nullptr, nullptr, 0), &PQclear};
    failure(extended.get());
    Result empty{PQexecParams(connection.get(), "", 0, nullptr, nullptr, nullptr, nullptr, 0), &PQclear};
    check(empty && PQresultStatus(empty.get()) == PGRES_EMPTY_QUERY);
    Result utility{
      PQexecParams(connection.get(), "SET application_name TO 'result_kind'", 0, nullptr, nullptr, nullptr, nullptr, 0),
      &PQclear};
    check(utility && PQresultStatus(utility.get()) == PGRES_COMMAND_OK);
    Result prepared{PQprepare(connection.get(), "probe", "SELECT 1/$1::int", 0, nullptr), &PQclear};
    check(prepared && PQresultStatus(prepared.get()) == PGRES_COMMAND_OK);
    const std::array arguments{"0"};
    Result failed{PQexecPrepared(connection.get(), "probe", 1, arguments.data(), nullptr, nullptr, 0), &PQclear};
    failure(failed.get());
    Result description{PQdescribePrepared(connection.get(), "probe"), &PQclear};
    check(description && PQresultStatus(description.get()) == PGRES_COMMAND_OK && PQnfields(description.get()) == 1);
    query(connection.get(), "BEGIN", PGRES_COMMAND_OK);
    query(connection.get(), "DECLARE cursor_kind CURSOR FOR SELECT 1 WHERE false", PGRES_COMMAND_OK);
    Result portal{PQdescribePortal(connection.get(), "cursor_kind"), &PQclear};
    check(portal && PQresultStatus(portal.get()) == PGRES_COMMAND_OK && PQnfields(portal.get()) == 1);
    auto zero = query(connection.get(), "FETCH ALL FROM cursor_kind", PGRES_TUPLES_OK);
    check(PQntuples(zero.get()) == 0 && PQnfields(zero.get()) == 1);
    failure(query(connection.get(), "SELECT 1/0", PGRES_FATAL_ERROR).get());
    check(PQtransactionStatus(connection.get()) == PQTRANS_INERROR);
    auto aborted = query(connection.get(), "SELECT 1", PGRES_FATAL_ERROR);
    check(std::string_view{PQresultErrorField(aborted.get(), PG_DIAG_SQLSTATE)} == "25P02");
    query(connection.get(), "ROLLBACK", PGRES_COMMAND_OK);
    query(connection.get(), "SELECT 1 WHERE false", PGRES_TUPLES_OK);
    connection.reset();
    failure(results[3].get());
    failure(extended.get());
    check(PQresultStatus(description.get()) == PGRES_COMMAND_OK);
  }
  std::printf("libpq result live controls passed: %u checks\n", checks);
}
