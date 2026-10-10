#include <libpq-fe.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

static void check(bool condition, const char *message)
{
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

static constexpr auto
  protected_sql = "SELECT encrypted, gss_authenticated, (SELECT ssl FROM pg_stat_ssl WHERE pid=pg_backend_pid()), 42 "
                  "FROM pg_stat_gssapi WHERE pid=pg_backend_pid()";

static std::atomic<unsigned> baseline_lookups{0};

static void baseline_cleanup(PGconn *, PGoauthBearerRequest *request)
{
  std::free(request->token);
}

static int baseline_token(PGauthData type, PGconn *connection, void *data)
{
  if (type != PQAUTHDATA_OAUTH_BEARER_TOKEN)
    return PQdefaultAuthDataHook(type, connection, data);
  ++baseline_lookups;
  auto *request = static_cast<PGoauthBearerRequest *>(data);
  request->token = static_cast<char *>(std::malloc(4));
  if (!request->token)
    return -1;
  std::memcpy(request->token, "abc", 4);
  request->cleanup = baseline_cleanup;
  return 1;
}

static void baseline(const std::string &number, const std::string &issuer)
{
  PQsetAuthDataHook(baseline_token);
  auto discovery = issuer + "/.well-known/openid-configuration";
  const std::array<const char *, 12> keys{
    "host",
    "hostaddr",
    "port",
    "user",
    "dbname",
    "sslmode",
    "gssencmode",
    "require_auth",
    "oauth_issuer",
    "oauth_client_id",
    "oauth_scope",
    nullptr};
  const std::array<const char *, 12> values{
    "localhost",
    "127.0.0.1",
    number.c_str(),
    "weave",
    "postgres",
    "disable",
    "require",
    "oauth",
    discovery.c_str(),
    "client:/ +&=",
    "read write",
    nullptr};
  auto *connection = PQconnectdbParams(keys.data(), values.data(), 0);
  if (PQstatus(connection) != CONNECTION_OK) {
    std::fprintf(stderr, "libpq: %s\n", PQerrorMessage(connection));
    std::_Exit(1);
  }
  auto *query = PQexec(connection, std::string{protected_sql}.c_str());
  check(
    PQresultStatus(query) == PGRES_TUPLES_OK && PQntuples(query) == 1 && PQnfields(query) == 4 &&
      std::string_view{PQgetvalue(query, 0, 0)} == "t" && std::string_view{PQgetvalue(query, 0, 1)} == "f" &&
      std::string_view{PQgetvalue(query, 0, 2)} == "f",
    "libpq confirms GSS encryption without TLS");
  PQclear(query);
  PQreset(connection);
  check(PQstatus(connection) == CONNECTION_OK && baseline_lookups == 2, "libpq protected OAuth reset");
  PQfinish(connection);
}

int main()
{
  std::string port, issuer;
  std::getline(std::cin, port);
  std::getline(std::cin, issuer);
  baseline(port, issuer);
  std::cout << "libpq protected OAuth connect/reset passed" << std::endl;
}
