#include <winsock2.h>
#include <libpq-fe.h>
#include <openssl/crypto.h>
#include "tls_certificates.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>

using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
using Cancel = std::unique_ptr<PGcancelConn, decltype(&PQcancelFinish)>;
using Clock = std::chrono::steady_clock;

static unsigned checks = 0;
static unsigned sessions = 0;
static unsigned cancellations = 0;
static unsigned stale_requests = 0;
static unsigned invalid_identity_controls = 0;
static std::atomic<unsigned> providers{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native certificate server check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

static int unused_password(char *, int, PGconn *)
{
  ++providers;
  return -1;
}

static void print_notice(void *, const PGresult *notice)
{
  std::fprintf(stderr, "%s", PQresultErrorMessage(notice));
}

static bool wait_socket(int socket, short events, Clock::time_point deadline)
{
  auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
  if (socket < 0 || remaining.count() <= 0)
    return false;

  WSAPOLLFD descriptor{static_cast<SOCKET>(socket), events, 0};
  return WSAPoll(&descriptor, 1, static_cast<int>(remaining.count())) > 0;
}

static bool finish_connect(PGconn *connection, bool reset)
{
  if (reset && !PQresetStart(connection))
    return false;

  auto deadline = Clock::now() + std::chrono::seconds{5};
  auto state = PGRES_POLLING_WRITING;
  for (;;) {
    if (state == PGRES_POLLING_OK)
      return true;
    if (state == PGRES_POLLING_FAILED)
      return false;
    if (!wait_socket(PQsocket(connection), state == PGRES_POLLING_READING ? POLLRDNORM : POLLWRNORM, deadline))
      return false;
    state = reset ? PQresetPoll(connection) : PQconnectPoll(connection);
  }
}

static Result query(PGconn *connection, const char *sql, bool nonblocking)
{
  if (!nonblocking)
    return {PQexec(connection, sql), PQclear};

  check(PQsendQuery(connection, sql) == 1);
  auto deadline = Clock::now() + std::chrono::seconds{12};
  int pending = 0;
  while ((pending = PQflush(connection)) == 1) {
    check(wait_socket(PQsocket(connection), POLLRDNORM | POLLWRNORM, deadline));
    check(PQconsumeInput(connection) == 1);
  }
  check(pending == 0);

  Result result{nullptr, PQclear};
  for (;;) {
    while (PQisBusy(connection)) {
      check(wait_socket(PQsocket(connection), POLLRDNORM, deadline));
      check(PQconsumeInput(connection) == 1);
    }
    Result next{PQgetResult(connection), PQclear};
    if (!next)
      break;
    check(!result);
    result = std::move(next);
  }
  check(result != nullptr);
  return result;
}

static bool dispatch(PGcancelConn *cancel, bool nonblocking)
{
  if (!nonblocking)
    return PQcancelBlocking(cancel) == 1;
  if (!PQcancelStart(cancel))
    return false;

  auto deadline = Clock::now() + std::chrono::seconds{5};
  auto state = PGRES_POLLING_WRITING;
  for (;;) {
    if (state == PGRES_POLLING_OK)
      return true;
    if (state == PGRES_POLLING_FAILED)
      return false;
    if (!wait_socket(PQcancelSocket(cancel), state == PGRES_POLLING_READING ? POLLRDNORM : POLLWRNORM, deadline))
      return false;
    state = PQcancelPoll(cancel);
  }
}

static void cancel_query(PGconn *connection, PGcancelConn *cancel, bool nonblocking, bool current)
{
  std::atomic<bool> ready{false};
  std::atomic<bool> dispatched{false};
  PQsetNoticeReceiver(
    connection,
    [](void *state, const PGresult *notice) {
      auto *message = PQresultErrorField(notice, PG_DIAG_MESSAGE_PRIMARY);
      if (message && std::string_view{message} == "native_certificate_cancel_ready")
        static_cast<std::atomic<bool> *>(state)->store(true);
    },
    &ready);

  std::thread worker{[&] {
    auto deadline = Clock::now() + std::chrono::seconds{5};
    while (!ready && Clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    if (ready)
      dispatched = dispatch(cancel, nonblocking);
  }};
  const char *sql = current
    ? "DO $$BEGIN RAISE NOTICE 'native_certificate_cancel_ready'; PERFORM pg_sleep(10); END$$"
    : "DO $$BEGIN RAISE NOTICE 'native_certificate_cancel_ready'; PERFORM pg_sleep(0.25); END$$";
  auto result = query(connection, sql, nonblocking);
  bool dispatched_before_completion = dispatched.load();
  worker.join();
  // A null receiver only inspects registration; install a nonborrowing receiver.
  PQsetNoticeReceiver(connection, print_notice, nullptr);
  check(PQsetNoticeReceiver(connection, nullptr, nullptr) == print_notice);

  if (!dispatched)
    std::fprintf(stderr, "Native cancel dispatch failed: %s", PQcancelErrorMessage(cancel));
  check(ready && dispatched && PQcancelStatus(cancel) == CONNECTION_OK);
  check(result != nullptr);
  if (current) {
    check(PQresultStatus(result.get()) == PGRES_FATAL_ERROR);
    auto *state = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
    check(state && std::string_view{state} == "57014");
    ++cancellations;
  } else {
    check(dispatched_before_completion && PQresultStatus(result.get()) == PGRES_COMMAND_OK);
    ++stale_requests;
  }
}

static void metadata(PGconn *connection, bool nonblocking, bool certificate, const std::string &version)
{
  check(PQstatus(connection) == CONNECTION_OK && PQsslInUse(connection));
  check(PQconnectionUsedPassword(connection) == 1);
  auto *protocol = PQsslAttribute(connection, "protocol");
  check(protocol && std::string_view{protocol} == version);
  auto result = query(
    connection,
    "SELECT ssl::text, (client_dn IS NOT NULL)::text, 42 FROM pg_stat_ssl WHERE pid=pg_backend_pid()",
    nonblocking);
  check(result && PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  check(PQntuples(result.get()) == 1 && PQnfields(result.get()) == 3);
  check(std::string_view{PQgetvalue(result.get(), 0, 0)} == "true");
  check(std::string_view{PQgetvalue(result.get(), 0, 1)} == (certificate ? "true" : "false"));
  check(std::string_view{PQgetvalue(result.get(), 0, 2)} == "42");
}

static void scenario(
  const fixture::Certificates &files,
  const std::string &port,
  const std::string &password,
  const std::string &version,
  bool request,
  const char *negotiation,
  const char *policy,
  const char *identity,
  bool nonblocking,
  bool strict = false)
{
  ++sessions;
  bool available = std::string_view{identity} == "file";
  bool invalid = std::string_view{identity} == "invalid";
  bool ignored = std::string_view{identity} == "ignored";
  bool disabled = std::string_view{policy} == "disable";
  bool success = std::string_view{policy} != "require" || (request && available);
  if (strict)
    success = available && !disabled;
  if (invalid)
    success = false;
  bool certificate = request && available && !disabled;
  auto missing_certificate = (files.directory / "missing.pem").string();
  auto missing_key = (files.directory / "missing.key").string();
  auto invalid_certificate = (files.directory / "invalid.pem").string();
  const char *certificate_path = available ? files.client.c_str() : missing_certificate.c_str();
  if (invalid || ignored)
    certificate_path = invalid_certificate.c_str();

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
    "sslcertmode",
    "channel_binding",
    "gssencmode",
    "sslnegotiation",
    "ssl_min_protocol_version",
    "ssl_max_protocol_version",
    "connect_timeout",
    static_cast<const char *>(nullptr)};
  const std::array values{
    "localhost",
    "127.0.0.1",
    port.c_str(),
    strict ? "weave_mtls" : "weave",
    "postgres",
    password.c_str(),
    "verify-full",
    files.ca.c_str(),
    certificate_path,
    available ? files.client_key.c_str() : missing_key.c_str(),
    policy,
    "require",
    "disable",
    negotiation,
    version.c_str(),
    version.c_str(),
    "5",
    static_cast<const char *>(nullptr)};
  Connection connection{
    nonblocking ? PQconnectStartParams(keys.data(), values.data(), 0)
                : PQconnectdbParams(keys.data(), values.data(), 0),
    PQfinish};
  check(connection != nullptr);
  bool connected = nonblocking ? finish_connect(connection.get(), false) : PQstatus(connection.get()) == CONNECTION_OK;
  if (!success) {
    check(!connected && PQstatus(connection.get()) == CONNECTION_BAD);
    check(std::string_view{PQerrorMessage(connection.get())}.find("certificate") != std::string_view::npos);
    return;
  }
  if (!connected)
    std::fprintf(stderr, "Native connection failed: %s", PQerrorMessage(connection.get()));
  check(connected);
  check(PQsetnonblocking(connection.get(), nonblocking ? 1 : 0) == 0);
  metadata(connection.get(), nonblocking, certificate, version);
  auto pid = PQbackendPID(connection.get());
  Cancel original{PQcancelCreate(connection.get()), PQcancelFinish};
  check(original && PQcancelStatus(original.get()) == CONNECTION_ALLOCATED);
  cancel_query(connection.get(), original.get(), nonblocking, true);
  metadata(connection.get(), nonblocking, certificate, version);

  if (nonblocking) {
    check(finish_connect(connection.get(), true));
  } else {
    PQreset(connection.get());
    check(PQstatus(connection.get()) == CONNECTION_OK);
  }
  check(PQbackendPID(connection.get()) != pid);
  check(PQsetnonblocking(connection.get(), nonblocking ? 1 : 0) == 0);
  metadata(connection.get(), nonblocking, certificate, version);
  PQcancelReset(original.get());
  check(PQcancelStatus(original.get()) == CONNECTION_ALLOCATED);
  cancel_query(connection.get(), original.get(), nonblocking, false);
  metadata(connection.get(), nonblocking, certificate, version);
  Cancel replacement{PQcancelCreate(connection.get()), PQcancelFinish};
  check(replacement && PQcancelStatus(replacement.get()) == CONNECTION_ALLOCATED);
  cancel_query(connection.get(), replacement.get(), nonblocking, true);
  metadata(connection.get(), nonblocking, certificate, version);
}

int main()
{
  check(PQlibVersion() == 180004);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  check(std::string_view{OpenSSL_version(OPENSSL_VERSION)} == "OpenSSL 3.6.5 29 Sep 2026");
  static fixture::Certificates files;
  {
    std::ofstream invalid(files.directory / "invalid.pem");
    invalid << "not a certificate\n";
    check(invalid.good());
  }
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string port, password, requests, tls;
  check(static_cast<bool>(std::getline(std::cin, port)));
  check(static_cast<bool>(std::getline(std::cin, password)));
  check(static_cast<bool>(std::getline(std::cin, requests)));
  check(static_cast<bool>(std::getline(std::cin, tls)));
  check((requests == "0" || requests == "1") && (tls == "12" || tls == "13"));
  bool request = requests == "1";
  std::string version = tls == "12" ? "TLSv1.2" : "TLSv1.3";
  auto previous = PQgetSSLKeyPassHook_OpenSSL();
  PQsetSSLKeyPassHook_OpenSSL(unused_password);
  unsigned cases = 0;
  const std::array negotiations{"postgres", "direct"};
  const std::array policies{"disable", "allow", "require"};
  const std::array identities{"file", "absent"};
  const std::array models{false, true};
  for (auto negotiation : negotiations) {
    for (auto policy : policies) {
      for (auto identity : identities) {
        for (bool nonblocking : models)
          scenario(files, port, password, version, request, negotiation, policy, identity, nonblocking);
        ++cases;
      }
    }
    for (bool nonblocking : models)
      scenario(files, port, password, version, request, negotiation, "disable", "ignored", nonblocking);
    ++cases;
    for (bool nonblocking : models) {
      scenario(files, port, password, version, request, negotiation, "allow", "invalid", nonblocking);
      ++invalid_identity_controls;
    }
    if (request) {
      const std::array strict_policies{"allow", "disable"};
      for (auto policy : strict_policies) {
        for (bool nonblocking : models)
          scenario(files, port, password, version, request, negotiation, policy, "file", nonblocking, true);
      }
    }
  }
  check(providers == 0 && cases == 14);
  PQsetSSLKeyPassHook_OpenSSL(previous);
  std::printf(
    "Native real certificate policy: %u checks, %u cases, %u sessions, %u observed query cancellations, "
    "%u active stale-request controls, %u invalid-identity controls, libpq=%d OpenSSL=%s\n",
    checks,
    cases,
    sessions,
    cancellations,
    stale_requests,
    invalid_identity_controls,
    PQlibVersion(),
    OpenSSL_version(OPENSSL_VERSION));
}
