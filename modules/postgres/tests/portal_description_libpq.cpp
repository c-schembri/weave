#include <libpq-fe.h>
#include "tls_certificates.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
static unsigned checks = 0;

static void check(bool condition)
{
  if (!condition) {
    std::fprintf(stderr, "libpq portal check failed: %u\n", checks + 1);
    std::exit(1);
  }
  ++checks;
}

static void command(PGconn *connection, const char *sql)
{
  Result result{PQexec(connection, sql), &PQclear};
  check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK);
}

static void columns(const PGresult *result, int format)
{
  if (result && PQnfields(result) == 2 && PQfformat(result, 0) != format)
    std::fprintf(stderr, "libpq observed portal format=%d expected=%d\n", PQfformat(result, 0), format);
  check(result && PQresultStatus(result) == PGRES_COMMAND_OK);
  check(PQnfields(result) == 2 && PQntuples(result) == 0 && PQnparams(result) == 0);
  check(std::string_view{PQfname(result, 0)} == "value" && PQftype(result, 0) == 23);
  check(PQfformat(result, 0) == format && PQfformat(result, 1) == format);
  check(PQfsize(result, 0) == 4 && PQfmod(result, 0) == -1);
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
    command(connection.get(), "BEGIN");
    command(connection.get(), "DECLARE probe BINARY CURSOR FOR SELECT 1::int AS value, NULL::text AS nullable");
    Result description{PQdescribePortal(connection.get(), "probe"), &PQclear};
    columns(description.get(), 0);
    Result first{PQexec(connection.get(), "FETCH 1 FROM probe"), &PQclear};
    check(first && PQresultStatus(first.get()) == PGRES_TUPLES_OK && PQntuples(first.get()) == 1);
    check(PQfformat(first.get(), 0) == 1);
    Result missing{PQdescribePortal(connection.get(), "missing"), &PQclear};
    check(missing && PQresultStatus(missing.get()) == PGRES_FATAL_ERROR);
    check(std::string_view{PQresultErrorField(missing.get(), PG_DIAG_SQLSTATE)} == "34000");
    command(connection.get(), "ROLLBACK");
    command(connection.get(), "BEGIN");
    Result query{
      PQexecParams(
        connection.get(),
        "SELECT 1::int AS value, NULL::text AS nullable",
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        0),
      &PQclear};
    check(query && PQresultStatus(query.get()) == PGRES_TUPLES_OK);
    Result unnamed{PQdescribePortal(connection.get(), ""), &PQclear};
    columns(unnamed.get(), 0);
    Result binary_query{
      PQexecParams(
        connection.get(),
        "SELECT 1::int AS value, NULL::text AS nullable",
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        1),
      &PQclear};
    check(binary_query && PQresultStatus(binary_query.get()) == PGRES_TUPLES_OK);
    Result binary{PQdescribePortal(connection.get(), ""), &PQclear};
    columns(binary.get(), 1);
    Result setting{
      PQexecParams(
        connection.get(),
        "SET application_name TO 'portal_no_data'",
        0,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        0),
      &PQclear};
    check(setting && PQresultStatus(setting.get()) == PGRES_COMMAND_OK);
    Result no_data{PQdescribePortal(connection.get(), ""), &PQclear};
    check(no_data && PQresultStatus(no_data.get()) == PGRES_COMMAND_OK);
    check(PQnfields(no_data.get()) == 0 && PQnparams(no_data.get()) == 0 && PQntuples(no_data.get()) == 0);
    command(connection.get(), "COMMIT");
    connection.reset();
    columns(description.get(), 0);
  }
  std::printf("libpq portal live controls passed: %u checks\n", checks);
}
