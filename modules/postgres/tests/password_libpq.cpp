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
using Text = std::unique_ptr<char, decltype(&PQfreemem)>;
static int checks = 0;

static void check(bool condition)
{
  if (!condition) {
    std::fprintf(stderr, "libpq password check %d failed\n", checks + 1);
    std::exit(1);
  }
  ++checks;
}

static void command(PGconn *connection, const char *sql)
{
  Result result{PQexec(connection, sql), &PQclear};
  check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK);
}

static void change(PGconn *connection, const char *user, const std::string &password)
{
  Result result{PQchangePassword(connection, user, password.c_str()), &PQclear};
  check(result && PQresultStatus(result.get()) == PGRES_COMMAND_OK);
  check(std::string_view{PQcmdStatus(result.get())} == "ALTER ROLE");
}

int main()
{
  static fixture::Certificates certificates;
#if !defined(_WIN32)
  std::error_code permissions;
  std::filesystem::permissions(
    certificates.client_key,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace,
    permissions);
  check(!permissions);
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

  auto connect = [&](const char *user, const std::string &credential, bool tls) {
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
      "127.0.0.1",
      address.c_str(),
      port.c_str(),
      user,
      "postgres",
      credential.c_str(),
      tls ? "verify-full" : "disable",
      certificates.ca.c_str(),
      certificates.client.c_str(),
      certificates.client_key.c_str(),
      "disable",
      "disable",
      "10",
      static_cast<const char *>(nullptr)};
    return Connection{PQconnectdbParams(keys.data(), values.data(), 0), &PQfinish};
  };

  auto baseline = connect("weave", password, false);
  check(baseline && PQstatus(baseline.get()) == CONNECTION_OK);
  const std::array passwords{
    std::string{"pencil"},
    std::string{},
    std::string{"\xc2\xaa"},
    std::string{"I\xc2\xadX"},
    std::string{"\a"},
    std::string{"\xff"},
    std::string{"\xd8\xa7x"},
    std::string{"\xc8\xa1"},
    std::string{"O'Reilly\\secret\n"}};
  for (std::size_t index = 0; index < passwords.size(); ++index) {
    Text scram{PQencryptPasswordConn(baseline.get(), passwords[index].c_str(), "user", "scram-sha-256"), &PQfreemem};
    Text md5{PQencryptPasswordConn(baseline.get(), passwords[index].c_str(), "user", "md5"), &PQfreemem};
    check(scram && md5);
    std::printf("Verifier: {\"case\":%zu,\"scram\":\"%s\",\"md5\":\"%s\"}\n", index, scram.get(), md5.get());
  }
  baseline.reset();

  struct Profile {
    const char *user;
    bool tls;
    const char *policy;
  };

  const std::array profiles{
    Profile{"weave", false, "SET password_encryption='scram-sha-256'"},
    Profile{"weave", true, "SET password_encryption='scram-sha-256'"},
    Profile{"weave_md5", true, "SET password_encryption=md5"}};
  for (auto profile : profiles) {
    auto connection = connect(profile.user, password, profile.tls);
    if (connection && PQstatus(connection.get()) != CONNECTION_OK)
      std::fprintf(stderr, "libpq connection failure: %s\n", PQerrorMessage(connection.get()));
    check(connection && PQstatus(connection.get()) == CONNECTION_OK);
    command(connection.get(), profile.policy);
    Text automatic{PQencryptPasswordConn(connection.get(), "pencil", profile.user, nullptr), &PQfreemem};
    check(static_cast<bool>(automatic));
    check(
      std::string_view{automatic.get()}.starts_with(
        profile.user == std::string_view{"weave_md5"} ? "md5" : "SCRAM-SHA-256$"));

    std::string replacement{"O'Reilly\xc2\xadX\\new-password"};
    change(connection.get(), profile.user, replacement);
    auto renewed = connect(profile.user, replacement, profile.tls);
    check(renewed && PQstatus(renewed.get()) == CONNECTION_OK);
    auto old = connect(profile.user, password, profile.tls);
    check(old && PQstatus(old.get()) == CONNECTION_BAD);
    Result injection{PQchangePassword(connection.get(), "weave\"; SELECT 1; --", "pencil"), &PQclear};
    check(injection && PQresultStatus(injection.get()) == PGRES_FATAL_ERROR);
    check(std::string_view{PQresultErrorField(injection.get(), PG_DIAG_SQLSTATE)} == "42704");
    change(connection.get(), profile.user, password);
    command(connection.get(), "BEGIN");
    Result failed{PQexec(connection.get(), "SELECT 1/0"), &PQclear};
    check(failed && PQresultStatus(failed.get()) == PGRES_FATAL_ERROR);
    Text aborted{PQencryptPasswordConn(connection.get(), "pencil", profile.user, nullptr), &PQfreemem};
    check(!aborted);
    command(connection.get(), "ROLLBACK");
  }
  std::printf("libpq password live controls passed: %d checks\n", checks);
}
