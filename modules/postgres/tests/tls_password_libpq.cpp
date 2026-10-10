#include <libpq-fe.h>
#include "tls_encrypted_certificates.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <thread>

using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
using Options = std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)>;
static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> calls{0};
static std::atomic<unsigned> invalid_requests{0};
static std::string selected_key;
static std::string selected_secret;
static bool deny = false;

static void check(bool condition, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Native key hook check failed at %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

static int hook(char *buffer, int size, PGconn *connection) noexcept
{
  ++calls;
  if (size <= 0)
    return 0;
  buffer[0] = '\0';

  Options options{PQconninfo(connection), PQconninfoFree};
  bool matched = false;
  if (options) {
    for (auto *option = options.get(); option->keyword; ++option) {
      if (std::string_view{option->keyword} == "sslkey")
        matched = option->val && option->val == selected_key;
    }
  }
  if (!matched) {
    ++invalid_requests;
    return 0;
  }
  if (deny || selected_secret.size() >= static_cast<std::size_t>(size))
    return 0;

  std::copy(selected_secret.begin(), selected_secret.end(), buffer);
  buffer[selected_secret.size()] = '\0';
  return static_cast<int>(selected_secret.size());
}

struct HookGuard {
  PQsslKeyPassHook_OpenSSL_type previous = PQgetSSLKeyPassHook_OpenSSL();

  HookGuard()
  {
    PQsetSSLKeyPassHook_OpenSSL(hook);
    check(PQgetSSLKeyPassHook_OpenSSL() == hook);
  }

  ~HookGuard()
  {
    PQsetSSLKeyPassHook_OpenSSL(previous);
    check(PQgetSSLKeyPassHook_OpenSSL() == previous);
  }
};

static Connection connect(
  const fixture::EncryptedCertificates &files,
  const std::string &port,
  const std::string &password,
  const std::string &address)
{
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
    files.certificates.ca.c_str(),
    files.certificates.client.c_str(),
    selected_key.c_str(),
    "require",
    "disable",
    "10",
    static_cast<const char *>(nullptr)};
  return Connection{PQconnectdbParams(keys.data(), values.data(), 0), PQfinish};
}

static void query(PGconn *connection)
{
  check(PQstatus(connection) == CONNECTION_OK && PQsslInUse(connection));
  Result result{PQexec(connection, "SELECT 73"), PQclear};
  check(result && PQresultStatus(result.get()) == PGRES_TUPLES_OK);
  check(PQntuples(result.get()) == 1 && PQnfields(result.get()) == 1);
  check(std::string_view{PQgetvalue(result.get(), 0, 0)} == "73");
}

int main()
{
  static fixture::EncryptedCertificates files{std::string(127, 'p')};
  std::printf(
    "%s\n%s\n%s\n",
    files.certificates.ca.c_str(),
    files.certificates.leaf.c_str(),
    files.certificates.private_key.c_str());
  std::fflush(stdout);
  std::string port, password, standby, address, local;
  check(static_cast<bool>(std::getline(std::cin, port)));
  check(static_cast<bool>(std::getline(std::cin, password)));
  check(static_cast<bool>(std::getline(std::cin, standby)));
  check(static_cast<bool>(std::getline(std::cin, address)));
  check(static_cast<bool>(std::getline(std::cin, local)));
  std::printf("Owned certificate fixture: %s\n", files.certificates.directory.string().c_str());
  std::printf("libpq version: %d\n", PQlibVersion());
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION && PQisthreadsafe());

#if !defined(_WIN32)
  const std::array private_keys{files.client_key, files.certificates.client_key};
  for (const auto &key : private_keys) {
    std::error_code error;
    std::filesystem::permissions(
      key,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace,
      error);
    check(!error);
  }
#endif

  {
    HookGuard guard;
    selected_key = files.client_key;
    selected_secret = std::string(127, 'p');
    auto first = connect(files, port, password, address);
    check(first && PQstatus(first.get()) == CONNECTION_OK);
    check(calls > 0);
    query(first.get());
    auto previous_calls = calls.load();
    PQreset(first.get());
    query(first.get());
    check(calls > previous_calls);
    first.reset();

    selected_key = files.certificates.client_key;
    previous_calls = calls.load();
    auto plain_key = connect(files, port, password, address);
    query(plain_key.get());
    check(calls == previous_calls);
    plain_key.reset();

    selected_key = files.client_key;
    const std::array wrong_secrets{std::string(127, 'w'), std::string(4096, 'p')};
    for (const auto &secret : wrong_secrets) {
      selected_secret = secret;
      previous_calls = calls.load();
      auto rejected = connect(files, port, password, address);
      check(rejected && PQstatus(rejected.get()) == CONNECTION_BAD);
      check(calls > previous_calls);
    }
    deny = true;
    previous_calls = calls.load();
    auto denied = connect(files, port, password, address);
    check(denied && PQstatus(denied.get()) == CONNECTION_BAD);
    check(calls > previous_calls);
    denied.reset();
    deny = false;
    selected_secret = std::string(127, 'p');

    std::array<std::thread, 4> workers;
    for (auto &worker : workers) {
      worker = std::thread([&] {
        for (int i = 0; i < 8; ++i) {
          auto connection = connect(files, port, password, address);
          check(connection && PQstatus(connection.get()) == CONNECTION_OK);
          query(connection.get());
        }
      });
    }
    for (auto &worker : workers)
      worker.join();
    check(invalid_requests == 0);
  }
  std::printf("Native key hook: %u checks passed, %u callback requests\n", checks.load(), calls.load());
}
