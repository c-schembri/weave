#include <libpq-fe.h>
#include "tls_certificates.hpp"
#include <openssl/crypto.h>
#include <source_location>
#include <cstdio>
#include <iostream>
#include <string_view>
#include <filesystem>

static unsigned checks = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Certificate native check failed: %s:%u\n", where.file_name(), where.line());
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
  check(argc == 6);
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  const std::string policy = argv[1];
  const std::string_view identity = argv[2];
  const bool request = std::string_view(argv[3]) == "1";
  const bool plaintext = std::string_view(argv[5]) == "plaintext";
  const std::string version = std::string_view(argv[4]) == "12" ? "TLSv1.2" : "TLSv1.3";
  static fixture::Certificates files;
#if !defined(_WIN32)
  std::error_code permissions_error;
  std::filesystem::permissions(
    files.client_key,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace,
    permissions_error);
  check(!permissions_error);
#endif

  std::printf(
    "%s\n%s\n%s\n%s\n%s\n",
    files.ca.c_str(),
    files.leaf.c_str(),
    files.private_key.c_str(),
    files.client.c_str(),
    files.client_key.c_str());
  std::fflush(stdout);
  std::string port, fallback;
  check(static_cast<bool>(std::getline(std::cin, port)) && static_cast<bool>(std::getline(std::cin, fallback)));

  std::string options = "user=weave dbname=postgres host=localhost hostaddr=127.0.0.1 port=" + port;
  options += " connect_timeout=5 gssencmode=disable sslcertmode=" + policy;
  options += plaintext ? " sslmode=disable sslnegotiation=postgres"
                       : " sslmode=verify-full sslnegotiation=" + std::string(argv[5]);
  options += " ssl_min_protocol_version=" + version + " ssl_max_protocol_version=" + version;
  options += " sslrootcert=" + keyword_value(files.ca);
  const bool available = identity == "file";
  const auto missing_certificate = (files.directory / "missing.pem").string();
  const auto missing_key = (files.directory / "missing.key").string();
  options += " sslcert=" + keyword_value(available ? files.client : missing_certificate);
  options += " sslkey=" + keyword_value(available ? files.client_key : missing_key);
  std::unique_ptr<PGconn, decltype(&PQfinish)> connection{PQconnectdb(options.c_str()), PQfinish};
  check(connection != nullptr);
  const bool success = policy != "require" || (available && request);
  if (!success) {
    check(PQstatus(connection.get()) == CONNECTION_BAD);
    check(std::string_view(PQerrorMessage(connection.get())).find("certificate") != std::string_view::npos);
  } else {
    if (PQstatus(connection.get()) != CONNECTION_OK)
      std::fprintf(stderr, "Certificate native connection: %s", PQerrorMessage(connection.get()));
    check(PQstatus(connection.get()) == CONNECTION_OK && PQsslInUse(connection.get()) == !plaintext);
    PQreset(connection.get());
    check(PQstatus(connection.get()) == CONNECTION_OK && PQsslInUse(connection.get()) == !plaintext);
    std::unique_ptr<PGcancelConn, decltype(&PQcancelFinish)> cancel{PQcancelCreate(connection.get()), PQcancelFinish};
    check(cancel != nullptr && PQcancelBlocking(cancel.get()) == 1);
  }
  std::printf(
    "Certificate native: %u checks, libpq %d, %s\n",
    checks,
    PQlibVersion(),
    OpenSSL_version(OPENSSL_VERSION));
}
