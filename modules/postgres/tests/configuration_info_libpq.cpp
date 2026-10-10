#include <libpq-fe.h>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

using Snapshot = std::map<std::string, std::optional<std::string>>;
static unsigned checks = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Native configuration check failed: %u\n", where.line());
    std::_Exit(1);
  }
}

static Snapshot own(PQconninfoOption *options)
{
  std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> owner{options, PQconninfoFree};
  check(options != nullptr);
  const std::array allowed{
    "service",
    "user",
    "connect_timeout",
    "dbname",
    "host",
    "hostaddr",
    "port",
    "client_encoding",
    "options",
    "application_name",
    "fallback_application_name",
    "keepalives",
    "keepalives_idle",
    "keepalives_interval",
    "keepalives_count",
    "tcp_user_timeout",
    "sslmode",
    "sslnegotiation",
    "sslcompression",
    "sslcert",
    "sslkey",
    "sslcertmode",
    "sslrootcert",
    "sslcrl",
    "sslcrldir",
    "sslsni",
    "requirepeer",
    "ssl_min_protocol_version",
    "ssl_max_protocol_version",
    "gssencmode",
    "krbsrvname",
    "gsslib",
    "gssdelegation",
    "replication",
    "target_session_attrs",
    "load_balance_hosts",
    "channel_binding",
    "require_auth",
    "oauth_issuer",
    "oauth_client_id",
    "oauth_scope"};
  Snapshot result;
  for (auto option = options; option->keyword; ++option) {
    bool selected = std::ranges::find(allowed, std::string_view{option->keyword}) != allowed.end();
    if (!selected)
      continue;
    result.emplace(option->keyword, option->val ? std::optional<std::string>{option->val} : std::nullopt);
  }
  check(result.size() == allowed.size());
  check(!result.contains("password") && !result.contains("sslpassword") && !result.contains("oauth_client_secret"));
  check(
    !result.contains("scram_client_key") && !result.contains("scram_server_key") && !result.contains("sslkeylogfile"));
  return result;
}

static void hex(std::string_view value)
{
  for (auto character : value)
    std::printf("%02x", static_cast<unsigned>(static_cast<unsigned char>(character)));
}

static void optional(const std::optional<std::string> &value)
{
  if (!value) {
    std::printf("null");
    return;
  }
  std::printf("\"");
  hex(*value);
  std::printf("\"");
}

static void defaults(std::string_view mode)
{
  auto snapshot = own(PQconndefaults());
  if (mode == "builtin") {
    check(snapshot.at("port") == "5432");
    check(snapshot.at("user") && !snapshot.at("user")->empty());
  } else if (mode != "missing") {
    check(snapshot.at("host") == "snapshot-host" && snapshot.at("port") == "6543");
    check(snapshot.at("user") == "snapshot-user" && snapshot.at("dbname") == "snapshot-database");
    check(snapshot.at("application_name") == "snapshot-app" && snapshot.at("connect_timeout") == "7");
    check(snapshot.at("options") == "-c work_mem=4096" && snapshot.at("sslmode") == "disable");
  }
  std::printf("{\"kind\":\"native-defaults\",\"mode\":\"");
  hex(mode);
  std::printf("\",\"fields\":{");
  bool first = true;
  for (const auto &[key, value] : snapshot) {
    std::printf("%s\"%s\":", first ? "" : ",", key.c_str());
    optional(value);
    first = false;
  }
  std::printf("}}\n");
}

static PGconn *connect(const char *port, const char *options, const char *application)
{
  const char *keys[]{
    "host",
    "hostaddr",
    "port",
    "user",
    "dbname",
    "sslmode",
    "gssencmode",
    "connect_timeout",
    "target_session_attrs",
    "options",
    "application_name",
    "client_encoding",
    "password",
    nullptr};
  std::string secret(137, 'p');
  const char *values[]{
    "127.0.0.1",
    "127.0.0.1",
    port,
    "test",
    "test",
    "disable",
    "disable",
    "23",
    "any",
    options,
    application,
    "UTF8",
    secret.c_str(),
    nullptr};
  auto connection = PQconnectdbParams(keys, values, 0);
  check(connection && PQstatus(connection) == CONNECTION_OK);
  return connection;
}

static Snapshot snapshot(PGconn *connection, unsigned phase)
{
  auto info = own(PQconninfo(connection));
  check(info.at("user") == "test" && info.at("dbname") == "test");
  check(info.at("host") == "127.0.0.1" && info.at("hostaddr") == "127.0.0.1");
  check(info.at("sslmode") == "disable" && info.at("gssencmode") == "disable");
  check(info.at("client_encoding") == "UTF8" && info.at("connect_timeout") == "23");
  check(info.at("options") == (phase == 2 ? "-c metadata.fixture=reset" : "-c metadata.fixture=initial"));
  check(info.at("application_name") == (phase == 2 ? "after-reset" : "before-reset"));
  std::printf("{\"phase\":%u,\"version\":%d,\"raw\":", phase, PQserverVersion(connection));
  auto raw = PQparameterStatus(connection, "server_version");
  optional(raw ? std::optional<std::string>{raw} : std::nullopt);
  std::printf(",\"options\":");
  optional(info.at("options"));
  std::printf(",\"empty\":");
  optional(PQparameterStatus(connection, "weave.fixture.empty"));
  std::printf(",\"value\":");
  optional(PQparameterStatus(connection, "weave.fixture.value"));
  std::printf(",\"absent\":");
  optional(std::nullopt);
  std::printf("}\n");
  return info;
}

static void session(const char *port)
{
  auto connection = connect(port, "-c metadata.fixture=initial", "before-reset");
  PQsetNoticeProcessor(
    connection,
    [](void *, const char *) {
    },
    nullptr);
  auto initial = snapshot(connection, 0);
  auto result = PQexec(connection, "NOOP");
  check(result && PQresultStatus(result) == PGRES_COMMAND_OK);
  PQclear(result);
  auto changed = snapshot(connection, 1);
  check(initial == changed);
  PQfinish(connection);
  connection = connect(port, "-c metadata.fixture=reset", "after-reset");
  auto reset = snapshot(connection, 2);
  PQfinish(connection);
  check(initial.at("application_name") == "before-reset" && reset.at("application_name") == "after-reset");
  check(initial.at("options") == "-c metadata.fixture=initial");
}

int main(int argc, char **argv)
{
  check(PQlibVersion() == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
  std::printf("libpq: %d\n", PQlibVersion());
  if (argc != 3)
    return 1;
  if (std::string_view{argv[2]} == "native")
    session(argv[1]);
  else
    defaults(argv[1]);
  std::printf("Configuration libpq controls passed: %u checks\n", checks);
}
