#include <libpq-fe.h>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include "tls_certificates.hpp"
#include <cstdio>
#include <iostream>
#include <source_location>
#include <thread>
#include <atomic>
#include <array>
#include <cstdlib>
#include <string_view>

using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
using Cancel = std::unique_ptr<PGcancelConn, decltype(&PQcancelFinish)>;
static unsigned checks = 0;
static unsigned cancellations = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native direct TLS control failed: %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

static void query(PGconn *connection)
{
  check(PQstatus(connection) == CONNECTION_OK && PQsslInUse(connection));

  Result result{PQexec(connection, "SELECT 42"), PQclear};
  check(result && PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  check(PQntuples(result.get()) == 1 && PQnfields(result.get()) == 1);
  check(std::string_view{PQgetvalue(result.get(), 0, 0)} == "42");
}

static void print_notice(void *, const PGresult *notice)
{
  std::fprintf(stderr, "%s", PQresultErrorMessage(notice));
}

static void cancel_query(PGconn *connection)
{
  std::atomic<bool> ready{false};
  PQsetNoticeReceiver(
    connection,
    [](void *state, const PGresult *notice) {
      auto *message = PQresultErrorField(notice, PG_DIAG_MESSAGE_PRIMARY);
      if (message && std::string_view{message} == "native_direct_cancel_ready")
        static_cast<std::atomic<bool> *>(state)->store(true);
    },
    &ready);
  Cancel cancel{PQcancelCreate(connection), PQcancelFinish};
  check(cancel != nullptr);

  std::atomic<bool> canceled{false};
  std::thread worker{[&] {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!ready && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    if (ready)
      canceled = PQcancelBlocking(cancel.get()) == 1;
  }};
  Result result{
    PQexec(connection, "DO $$BEGIN RAISE NOTICE 'native_direct_cancel_ready'; PERFORM pg_sleep(6); END$$"),
    PQclear};
  worker.join();

  // Null only inspects the receiver; do not retain the borrowed ready flag.
  PQsetNoticeReceiver(connection, print_notice, nullptr);
  check(PQsetNoticeReceiver(connection, nullptr, nullptr) == print_notice);
  check(ready && canceled && PQcancelStatus(cancel.get()) == CONNECTION_OK);
  check(result && PQresultStatus(result.get()) == PGRES_FATAL_ERROR);
  auto *state = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
  check(state && std::string_view{state} == "57014");
  query(connection);
  ++cancellations;
}

int main()
{
  static fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);

  std::string port, password, standby, address, local;
  check(static_cast<bool>(std::getline(std::cin, port)));
  check(static_cast<bool>(std::getline(std::cin, password)));
  check(static_cast<bool>(std::getline(std::cin, standby)));
  check(static_cast<bool>(std::getline(std::cin, address)));
  check(static_cast<bool>(std::getline(std::cin, local)));
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);

#if !defined(_WIN32)
  std::error_code error;
  std::filesystem::permissions(
    files.client_key,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace,
    error);
  check(!error);
#endif

  const std::array modes{"postgres", "direct"};
  for (const auto *mode : modes) {
    const std::array keys{
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
      "sslnegotiation",
      "connect_timeout",
      static_cast<const char *>(nullptr)};
    const std::array values{
      "localhost",
      address.c_str(),
      port.c_str(),
      "weave",
      "postgres",
      password.c_str(),
      "verify-full",
      files.ca.c_str(),
      files.client.c_str(),
      files.client_key.c_str(),
      "require",
      "disable",
      mode,
      "5",
      static_cast<const char *>(nullptr)};

    Connection connection{PQconnectdbParams(keys.data(), values.data(), 0), PQfinish};
    if (PQstatus(connection.get()) != CONNECTION_OK)
      std::fprintf(stderr, "%s", PQerrorMessage(connection.get()));
    query(connection.get());
    PQreset(connection.get());
    query(connection.get());
    cancel_query(connection.get());
  }
  std::printf(
    "Native direct/legacy TLS: %u checks, %u observed query cancellations, libpq=%d OpenSSL=%s\n",
    checks,
    cancellations,
    PQlibVersion(),
    OpenSSL_version(OPENSSL_VERSION));
}
