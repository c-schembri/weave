#include <libpq-fe.h>
#include "tls_certificates.hpp"
#include <openssl/crypto.h>
#include <cstdio>
#include <iostream>
#include <string_view>

static unsigned checks = 0;

static void check(bool value)
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "SNI native check failed: %u\n", checks);
    std::exit(1);
  }
}

static std::string keyword_value(std::string_view input)
{
  std::string output{"'"};
  for (char value : input) {
    if (value == '\'' || value == '\\')
      output += '\\';
    output += value;
  }
  output += '\'';
  return output;
}

int main(int argc, char **argv)
{
  check(argc == 5);
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  std::string mode = argv[1];
  std::string policy = argv[2];
  std::string version = std::string_view(argv[3]) == "12" ? "TLSv1.2" : "TLSv1.3";
  static fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string port, fallback;
  check(static_cast<bool>(std::getline(std::cin, port)) && static_cast<bool>(std::getline(std::cin, fallback)));

  std::string options = "user=weave dbname=postgres host=";
  options += mode == "hostname" ? "wrong.invalid" : "localhost";
  options += " hostaddr=127.0.0.1 port=" + port + " connect_timeout=5 sslmode=verify-full gssencmode=disable";
  options += " sslsni=" + policy + " sslnegotiation=" + argv[4];
  options += " ssl_min_protocol_version=" + version + " ssl_max_protocol_version=" + version;
  options += " sslrootcert=" + keyword_value(mode == "untrusted" ? files.untrusted : files.ca);
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{PQconnectdb(options.c_str()), PQfinish};
  check(connection != nullptr);
  if (mode == "hostname" || mode == "untrusted") {
    check(PQstatus(connection.get()) == CONNECTION_BAD);
    check(std::string_view(PQerrorMessage(connection.get())).find("certificate") != std::string_view::npos);
  } else {
    check(PQstatus(connection.get()) == CONNECTION_OK && PQsslInUse(connection.get()) == 1);
    PQreset(connection.get());
    check(PQstatus(connection.get()) == CONNECTION_OK && PQsslInUse(connection.get()) == 1);
    std::unique_ptr<PGcancelConn, decltype(&PQcancelFinish)> cancel{PQcancelCreate(connection.get()), PQcancelFinish};
    check(cancel != nullptr && PQcancelBlocking(cancel.get()) == 1);
  }
  std::printf("SNI native: %u checks, libpq %d, %s\n", checks, PQlibVersion(), OpenSSL_version(OPENSSL_VERSION));
}
