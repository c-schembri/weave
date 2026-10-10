#include <weave/port.hpp>

#include <libpq-fe.h>
#include <openssl/crypto.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <string_view>

static unsigned checks = 0;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native availability check failed at line %u\n", location.line());
    std::_Exit(EXIT_FAILURE);
  }
}

int main(int argc, char **argv)
{
  check(argc == 13);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  check(PQlibVersion() == std::atoi(argv[11]));

  auto port = weave::parse_port(argv[1]);
  auto closed_port = weave::parse_port(argv[4]);
  check(port && closed_port);

  std::string_view mode = argv[2];
  check(std::string_view{argv[3]} == "native");
  const bool local = mode.starts_with("local");
  const bool retry = mode == "retry";
  const bool secured = mode == "tls" || mode == "password" || mode == "mtls" || mode == "untrusted" ||
    mode == "no_certificate";
  const bool identity = secured && mode != "no_certificate";

  std::string ports = std::to_string(mode == "refused" ? *closed_port : *port);
  std::string addresses = argv[9];
  if (retry) {
    ports = std::to_string(*closed_port) + "," + ports;
    addresses += "," + std::string{argv[9]};
  }

  const char *host = local ? argv[10] : "localhost";
  if (retry)
    host = "localhost,localhost";
  const char *encryption = "disable";
  if (mode == "gss_prefer")
    encryption = "prefer";
  else if (mode.starts_with("gss_") && mode != "gss_auth")
    encryption = "require";

  std::string application = "health_" + std::string{mode} + "_native";
  const std::array<const char *, 17> keys{
    "host",
    "hostaddr",
    "port",
    "user",
    "dbname",
    "password",
    "sslmode",
    "gssencmode",
    "connect_timeout",
    "target_session_attrs",
    "sslrootcert",
    "sslcert",
    "sslkey",
    "sslcertmode",
    "application_name",
    "krbsrvname",
    nullptr};
  const std::array<const char *, 17> values{
    host,
    local ? "" : addresses.c_str(),
    ports.c_str(),
    argv[8],
    mode == "wrong_database" ? "absent_database" : "postgres",
    "",
    secured ? "verify-full" : "disable",
    encryption,
    "3",
    "any",
    argv[5],
    identity ? argv[6] : "",
    identity ? argv[7] : "",
    identity ? "require" : "disable",
    application.c_str(),
    mode == "gss_wrong_service" ? "absent_service" : "postgres",
    nullptr};

  auto status = PQpingParams(keys.data(), values.data(), 0);
  const bool unavailable = mode == "refused" || mode == "untrusted" || mode == "gss_missing" ||
    mode == "gss_wrong_service";
  check(status == (unavailable ? PQPING_NO_RESPONSE : PQPING_OK));
  std::printf("Native probe status: %d\n", static_cast<int>(status));
  std::printf("Live availability controls passed: %u checks; %s\n", checks, OpenSSL_version(OPENSSL_VERSION));
}
