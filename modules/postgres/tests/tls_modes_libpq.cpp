#include <libpq-fe.h>
#include "tls_certificates.hpp"
#include <cstdio>
#include <iostream>
#include <source_location>

static unsigned checks = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native TLS mode check failed at %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

int main(int argc, char **argv)
{
  check(argc == 2);
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string port;
  check(static_cast<bool>(std::getline(std::cin, port)));
  const std::string mode = argv[1];
  auto policy = mode.substr(0, mode.find('-'));
  if (mode.starts_with("verify-full"))
    policy = "verify-full";
  else if (mode.starts_with("verify-ca"))
    policy = "verify-ca";
  const bool ca = policy.starts_with("verify") || mode == "require-ca";
  const auto &root = mode == "verify-ca-untrusted" ? files.untrusted : files.ca;
  const bool wrong = mode == "verify-full-wrong" || mode == "verify-ca" || mode == "require-ca";
  const char *keys[]{
    "user",
    "dbname",
    "host",
    "hostaddr",
    "port",
    "sslmode",
    "sslrootcert",
    "gssencmode",
    "connect_timeout",
    nullptr};
  const char *values[]{
    "weave",
    "postgres",
    wrong ? "wrong.invalid" : "localhost",
    "127.0.0.1",
    port.c_str(),
    policy.c_str(),
    ca ? root.c_str() : "",
    "disable",
    "3",
    nullptr};
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{PQconnectdbParams(keys, values, 0), PQfinish};
  check(connection != nullptr);
  const bool failed = mode == "verify-full-wrong" || mode == "verify-ca-untrusted" || mode == "require-n";
  check((PQstatus(connection.get()) == CONNECTION_OK) != failed);
  if (!failed) {
    const bool plain = mode == "disable" || mode == "allow" || mode == "prefer-n" || mode == "prefer-broken";
    check((PQsslInUse(connection.get()) != 0) != plain);
    std::unique_ptr<PGresult, decltype(&PQclear)> rows{PQexec(connection.get(), "SELECT 42"), PQclear};
    check(rows && PQresultStatus(rows.get()) == PGRES_TUPLES_OK);
    check(PQntuples(rows.get()) == 1 && std::string_view{PQgetvalue(rows.get(), 0, 0)} == "42");
  }
  std::printf(
    "TLS modes: 1 sessions, %u checks; libpq %d, %s\n",
    checks,
    PQlibVersion(),
    OpenSSL_version(OPENSSL_VERSION));
}
