#include <weave/postgres.hpp>
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>
#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

namespace pg = weave::pg;
static std::filesystem::path directory;

static std::string path_text(const std::filesystem::path &path)
{
  auto text = path.u8string();
  return std::string{text.begin(), text.end()};
}

static void write(const std::filesystem::path &path, std::string_view text)
{
  std::ofstream output(path, std::ios::binary);
  output << text;
  output.close();
  REQUIRE(output);

  std::error_code error;
  std::filesystem::permissions(
    path,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace,
    error);
  REQUIRE_FALSE(error);
}

TEST_CASE("LDAP service loading is disabled unless explicitly requested")
{
  auto service = directory / "ldap-disabled.conf";
  write(service, "[sample]\nldap://127.0.0.1:1/cn=x?description?base?(cn=*)\nuser=fallback\n");
  pg::ConfigSources sources{.environment = false, .user_files = false, .service_file = path_text(service)};
  auto result = pg::Options::load("service=sample", sources);
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_not_supported);
}

TEST_CASE("PostgreSQL peer-name loading is explicit and preserves service precedence")
{
  pg::ConfigSources sources{.environment = false, .user_files = false};
  auto empty = pg::Options::load("user=app requirepeer=''", sources);
  REQUIRE(empty);
  CHECK_FALSE(empty->required_peer_user);
#ifdef _WIN32
  auto unsupported = pg::Options::load("user=app requirepeer=postgres", sources);
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error() == std::errc::operation_not_supported);
#else
  auto account = getpwuid(geteuid());
  REQUIRE(account);
  REQUIRE(account->pw_name);
  std::string name{account->pw_name};
  auto service = directory / "peer.conf";
  write(service, "[peer]\nuser=app\nrequirepeer=" + name + "\n");
  sources.service_file = path_text(service);
  auto loaded = pg::Options::load("service=peer", sources);
  REQUIRE(loaded);
  CHECK(loaded->required_peer_user == geteuid());
  auto overridden = pg::Options::load("service=peer requirepeer=''", sources);
  REQUIRE(overridden);
  CHECK_FALSE(overridden->required_peer_user);
  auto unknown = pg::Options::load("user=app requirepeer=weave-missing-user-95714c2d", sources);
  REQUIRE_FALSE(unknown);
  CHECK(unknown.error() == std::errc::no_such_file_or_directory);
#endif
}

TEST_CASE("PostgreSQL ambient peer policy can be explicitly disabled")
{
  pg::ConfigSources sources{.environment = true, .user_files = false};
  auto loaded = pg::Options::load("user=app", sources);
#ifdef _WIN32
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error() == std::errc::operation_not_supported);
#else
  REQUIRE(loaded);
  CHECK(loaded->required_peer_user == geteuid());
#endif
  auto overridden = pg::Options::load("user=app requirepeer=''", sources);
  REQUIRE(overridden);
  CHECK_FALSE(overridden->required_peer_user);
}

TEST_CASE("SCRAM keys load from explicit services and connection strings take precedence")
{
  constexpr std::string_view client = "Codc/xpog7iN42jX2M/Kzb0EYyzwnhS38XdLARZUmGc=";
  constexpr std::string_view server = "2MBPkZDzn+Pg6MZjW44WbB3uM/DpW2q3cpTa0wpqb8g=";
  auto service = directory / "scram-keys.conf";
  write(
    service,
    "[keys]\nscram_client_key=" + std::string(client) + "\nscram_server_key=" + std::string(server) + "\n");
  pg::ConfigSources sources{.environment = false, .user_files = false, .service_file = path_text(service)};
  auto loaded = pg::Options::load("service=keys", sources);
  REQUIRE(loaded);
  REQUIRE(loaded->scram_client_key);
  REQUIRE(loaded->scram_server_key);
  auto expected_client = pg::ScramKey::parse(client);
  auto expected_server = pg::ScramKey::parse(server);
  REQUIRE(expected_client);
  REQUIRE(expected_server);
  CHECK(std::ranges::equal(loaded->scram_client_key->bytes(), expected_client->bytes()));
  CHECK(std::ranges::equal(loaded->scram_server_key->bytes(), expected_server->bytes()));

  loaded = pg::Options::load("service=keys scram_server_key=" + std::string(client), sources);
  REQUIRE(loaded);
  REQUIRE(loaded->scram_server_key);
  CHECK(std::ranges::equal(loaded->scram_server_key->bytes(), expected_client->bytes()));
  auto rejected = pg::Options::load("service=keys scram_client_key=''", sources);
  CHECK((!rejected && rejected.error() == std::errc::invalid_argument));
}

TEST_CASE("PostgreSQL explicit local directories match password file host names without localhost substitution")
{
  auto file = directory / "local-passwords";
  write(
    file,
    "localhost:5432:postgres:local_user:incorrect\n/weave-local:5432:postgres:local_user:directory-secret\n"
    "C\\:/pg:5432:postgres:local_user:drive-secret\n");
  pg::ConfigSources sources{.environment = false, .user_files = false, .password_file = path_text(file)};
  auto options = pg::Options::load("host=/weave-local user=local_user dbname=postgres sslmode=disable", sources);
  REQUIRE(options);
  REQUIRE(options->hosts.size() == 1);
  CHECK(options->hosts.front().password == "directory-secret");
#if defined(_WIN32)
  options = pg::Options::load("postgres://local_user@x/postgres?host=C%3A%2Fpg&sslmode=disable", sources);
  REQUIRE(options);
  CHECK(options->hosts.front().password == "drive-secret");
#endif
}

TEST_CASE("PostgreSQL service precedence preserves literal values and uses the first matching section")
{
  auto service = directory / std::filesystem::path{std::u8string{u8"service-\u00e9.conf"}};
  auto system = directory / "system.conf";
  write(
    service,
    "# fixture\r\n[other]\r\nunknown=ignored\r\n[sample]\r\nhost=first,second\r\n"
    "user=service_user\r\ndbname=service_database\r\napplication_name=service_app\r\napplication_name=ignored\r\n"
    "password=service_password\r\nsslmode=disable\r\nkeepalives_idle=60\r\nkeepalives_interval=5\r\n"
    "keepalives_count=3\r\nload_balance_hosts=random\r\n[following]\r\nunknown=ignored\r\n");
  write(system, "[sample]\nuser=system_user\n[system]\nuser=system_user\nsslmode=disable\n");

  pg::ConfigSources sources{
    .environment = false,
    .user_files = false,
    .service_file = path_text(service),
    .system_service_file = path_text(system)};
  auto loaded = pg::Options::load("service=sample application_name=explicit", sources);
  REQUIRE(loaded);
  CHECK(loaded->user == "service_user");
  CHECK(loaded->application_name == "explicit");
  CHECK(loaded->password == "service_password");
  CHECK(loaded->hosts.size() == 2);
  CHECK(loaded->plaintext);
  CHECK(loaded->keep_alive.enabled);
  CHECK(loaded->keep_alive.idle == std::chrono::seconds{60});
  CHECK(loaded->keep_alive.interval == std::chrono::seconds{5});
  CHECK(loaded->keep_alive.probes == 3);
  CHECK(loaded->host_balance == pg::HostBalance::random);

  loaded = pg::Options::load("service=sample keepalives=0 keepalives_idle=90 load_balance_hosts=disable", sources);
  REQUIRE(loaded);
  CHECK_FALSE(loaded->keep_alive.enabled);
  CHECK(loaded->keep_alive.idle == std::chrono::seconds{90});
  CHECK(loaded->keep_alive.interval == std::chrono::seconds{5});
  CHECK(loaded->host_balance == pg::HostBalance::ordered);

  loaded = pg::Options::load("service=sample", sources);
  REQUIRE(loaded);
  CHECK(loaded->application_name == "service_app");
  loaded = pg::Options::load("service=system", sources);
  REQUIRE(loaded);
  CHECK(loaded->user == "system_user");
  loaded = pg::Options::load("service=sample user=explicit password=explicit", sources);
  REQUIRE(loaded);
  CHECK(loaded->user == "explicit");
  CHECK(loaded->password == "explicit");

  CHECK_FALSE(pg::Options::load("service=absent", sources));
  sources.service_file = path_text(directory / "missing");
  CHECK_FALSE(pg::Options::load("service=system", sources));
  sources.service_file = path_text(service);
  write(service, "[sample]\npassword='literal quotes'\nuser=user\n");
  loaded = pg::Options::load("service=sample", sources);
  REQUIRE(loaded);
  CHECK(loaded->password == "'literal quotes'");

  write(service, "[sample]\nservice=nested\n");
  auto nested = pg::Options::load("service=sample", sources);
  REQUIRE_FALSE(nested);
  CHECK(nested.error() == std::errc::operation_not_supported);
  write(service, "[sample]\nunknown=invalid\n");
  CHECK_FALSE(pg::Options::load("service=sample", sources));
}

TEST_CASE("PostgreSQL authentication and protocol configuration uses explicit precedence")
{
  auto loaded = pg::Options::load("", {.user_files = false});
  REQUIRE(loaded);
  CHECK(loaded->authentication.methods == std::vector{pg::Authentication::scram_sha256});
  CHECK(loaded->min_protocol == pg::ProtocolVersion::v30);
  CHECK(loaded->max_protocol == pg::ProtocolVersion::v32);

  auto service = directory / "authentication-service.conf";
  write(service, "[sample]\nrequire_auth=!none\nmin_protocol_version=3.0\nmax_protocol_version=3.0\n");
  pg::ConfigSources sources{.user_files = false, .service_file = path_text(service)};
  loaded = pg::Options::load("service=sample", sources);
  REQUIRE(loaded);
  CHECK(loaded->authentication.exclude);
  CHECK(loaded->authentication.methods == std::vector{pg::Authentication::none});
  CHECK(loaded->max_protocol == pg::ProtocolVersion::v30);
  loaded = pg::Options::load(
    "service=sample require_auth=md5 min_protocol_version=3.2 max_protocol_version=latest",
    sources);
  REQUIRE(loaded);
  CHECK_FALSE(loaded->authentication.exclude);
  CHECK(loaded->authentication.methods == std::vector{pg::Authentication::md5});
  CHECK_FALSE(loaded->allow_md5_password);
  CHECK(loaded->min_protocol == pg::ProtocolVersion::v32);
  CHECK(loaded->max_protocol == pg::ProtocolVersion::v32);
  loaded = pg::Options::load("service=sample min_protocol_version=3.2", sources);
  CHECK((!loaded && loaded.error() == std::errc::invalid_argument));
  loaded = pg::Options::load("require_auth=gss", {.user_files = false});
#if defined(_WIN32) || defined(WEAVE_POSTGRES_TEST_GSSAPI)
  REQUIRE(loaded);
  CHECK(loaded->authentication.methods == std::vector{pg::Authentication::gss});
  CHECK_FALSE(loaded->gss);
#else
  CHECK((!loaded && loaded.error() == std::errc::operation_not_supported));
#endif
}

TEST_CASE("PostgreSQL password files select first matches per host with escapes and literal wildcard handling")
{
  auto password = directory / "password";
  write(
    password,
    "# fixture\nmalformed\nfirst:5432:database:user:first\\:secret\\\\end\n"
    "first:5432:database:user:ignored\nsecond:5432:database:user:second_secret\n"
    "\\:\\:1:5432:database:user:ipv6_secret\n\\*:5432:database:user:literal_star\n"
    "*:5432:database:user:wildcard_secret\n");
  pg::ConfigSources sources{.environment = false, .user_files = false, .password_file = path_text(password)};
  auto loaded = pg::Options::load("host=first,second,::1,* user=user dbname=database sslmode=disable", sources);
  REQUIRE(loaded);
  CHECK(loaded->password.empty());
  REQUIRE(loaded->hosts.size() == 4);
  CHECK(loaded->hosts[0].password == "first:secret\\end");
  CHECK(loaded->hosts[1].password == "second_secret");
  CHECK(loaded->hosts[2].password == "ipv6_secret");
  CHECK(loaded->hosts[3].password == "literal_star");

  loaded = pg::Options::load("host=other user=user dbname=database sslmode=disable", sources);
  REQUIRE(loaded);
  CHECK(loaded->hosts[0].password == "wildcard_secret");
  loaded = pg::Options::load("host=first,second user=user dbname=database password=global sslmode=disable", sources);
  REQUIRE(loaded);
  CHECK(loaded->password == "global");
  CHECK_FALSE(loaded->hosts[0].password);
  CHECK_FALSE(loaded->hosts[1].password);

  write(password, "*:5432:user:user:default_database\n");
  loaded = pg::Options::load("user=user", sources);
  REQUIRE(loaded);
  CHECK(loaded->password == "default_database");
  sources.password_file = path_text(directory / "missing");
  CHECK_FALSE(pg::Options::load("user=user", sources));
  CHECK(pg::Options::load("user=user password=explicit", sources));
  CHECK_FALSE(pg::Options::parse("service=sample"));
  CHECK_FALSE(pg::Options::parse("passfile=password"));
}

TEST_CASE("PostgreSQL configuration files have bounded snapshots and reject nonregular or unsafe password files")
{
  auto file = directory / "bounded";
  pg::ConfigSources sources{.environment = false, .user_files = false, .service_file = path_text(file)};
  write(file, std::string(1024 * 1024 + 1, 'a'));
  auto oversized = pg::Options::load("service=sample", sources);
  REQUIRE_FALSE(oversized);
  CHECK(oversized.error() == std::errc::file_too_large);
  write(file, std::string{"[sample]\nuser=a\0b\n", 18});
  CHECK_FALSE(pg::Options::load("service=sample", sources));

  sources = {.environment = false, .user_files = false, .password_file = path_text(directory)};
  CHECK_FALSE(pg::Options::load("user=user", sources));
  write(file, "*:5432:user:user:secret\n");
#ifndef _WIN32
  std::error_code error;
  std::filesystem::permissions(
    file,
    std::filesystem::perms::owner_all | std::filesystem::perms::group_read,
    std::filesystem::perm_options::replace,
    error);
  REQUIRE_FALSE(error);
  sources.password_file = path_text(file);
  auto insecure = pg::Options::load("user=user", sources);
  REQUIRE_FALSE(insecure);
  CHECK(insecure.error() == std::errc::permission_denied);
#endif
  CHECK_FALSE(
    pg::Options::load(
      "user=user",
      {.environment = false, .user_files = false, .password_file = std::string{"path\0suffix", 11}}));
}

TEST_CASE("PostgreSQL physical replication selects replication passwords, not ordinary database credentials")
{
  auto file = directory / "replication-password";
  write(
    file,
    "*:5432:database:user:ordinary\nfirst:5432:replication:user:physical_first\n"
    "second:5432:replication:user:physical_second\n*:5432:replication:user:physical_default\n");
  pg::ConfigSources sources{.environment = false, .user_files = false, .password_file = path_text(file)};
  auto physical = pg::Options::load("host=first,second user=user dbname=database replication=true", sources);
  REQUIRE(physical);
  REQUIRE(physical->hosts.size() == 2);
  CHECK(physical->hosts[0].password == "physical_first");
  CHECK(physical->hosts[1].password == "physical_second");

  auto default_host = pg::Options::load("user=user dbname=database replication=1", sources);
  REQUIRE(default_host);
  CHECK(default_host->password == "physical_default");

  const std::array ordinary_modes{"false", "database"};
  for (auto mode : ordinary_modes) {
    auto ordinary = pg::Options::load(
      "host=first,second user=user dbname=database replication=" + std::string(mode),
      sources);
    REQUIRE(ordinary);
    REQUIRE(ordinary->hosts.size() == 2);
    CHECK(ordinary->hosts[0].password == "ordinary");
    CHECK(ordinary->hosts[1].password == "ordinary");
  }
}

TEST_CASE("PostgreSQL loading explicitly reads environment defaults and startup settings")
{
  auto loaded = pg::Options::load("", {.environment = true, .user_files = false});
  REQUIRE(loaded);
  CHECK(loaded->user == "env_user");
  CHECK(loaded->password == "env_password");
  CHECK(loaded->application_name == "env_app");
  CHECK(loaded->host_balance == pg::HostBalance::random);
  REQUIRE(loaded->hosts.size() == 1);
  CHECK(loaded->hosts[0].name == "env_host");
  const std::vector<std::pair<std::string, std::string>> settings{
    {"DateStyle", "ISO, MDY"},
    {"TimeZone", "UTC"},
    {"geqo", "off"}};
  CHECK(loaded->settings == settings);

  auto service = directory / "environment-service";
  write(service, "[sample]\nuser=service_user\npassword=service_password\nsslmode=disable\n");
  pg::ConfigSources sources{.environment = true, .user_files = false, .service_file = path_text(service)};
  loaded = pg::Options::load("service=sample", sources);
  REQUIRE(loaded);
  CHECK(loaded->user == "service_user");
  CHECK(loaded->password == "service_password");
  loaded = pg::Options::load("service=sample user=explicit", sources);
  REQUIRE(loaded);
  CHECK(loaded->user == "explicit");

  loaded = pg::Options::load("", {.environment = false, .user_files = false});
  REQUIRE(loaded);
  CHECK_FALSE(loaded->user.empty());
  CHECK(loaded->password.empty());
  CHECK(loaded->settings.empty());
  CHECK(loaded->hosts.empty());
  CHECK(loaded->host_balance == pg::HostBalance::ordered);
}

TEST_CASE("PostgreSQL user-file discovery uses configured HOME or APPDATA with database defaults")
{
  auto home = directory;
#ifdef _WIN32
  home /= "postgresql";
  std::error_code error;
  std::filesystem::create_directory(home, error);
  REQUIRE_FALSE(error);
#endif
  write(home / ".pg_service.conf", "[defaults]\nhost=default_host\nsslmode=disable\n");
#ifdef _WIN32
  auto password = home / "pgpass.conf";
#else
  auto password = home / ".pgpass";
#endif
  write(password, "*:5432:database:user:default_secret\n");
  auto loaded = pg::Options::load("service=defaults password='' user=user dbname=database");
  REQUIRE(loaded);
  REQUIRE(loaded->hosts.size() == 1);
  CHECK(loaded->hosts[0].name == "default_host");
  CHECK(loaded->hosts[0].password == "default_secret");
}

TEST_CASE("PostgreSQL ambient security overrides are explicit and cannot silently downgrade")
{
  auto insecure = pg::Options::load("user=user", {.environment = true, .user_files = false});
  if (std::getenv("PGSSLMODE")) {
    REQUIRE(insecure);
    CHECK(insecure->tls_mode == pg::TlsMode::prefer);
  } else {
    REQUIRE_FALSE(insecure);
    CHECK(insecure.error() == std::errc::operation_not_supported);
  }
  auto secure = pg::Options::load("user=user sslmode=verify-full", {.environment = true, .user_files = false});
  REQUIRE(secure);
  CHECK_FALSE(secure->plaintext);
}

TEST_CASE("PostgreSQL ambient service and password files retain per-host selection and system fallback")
{
  write(
    directory / "env-service.conf",
    "[env-service]\nhost=first,second\nuser=user\ndbname=database\npassword=\nsslmode=disable\n");
  write(directory / "pg_service.conf", "[system]\nuser=system_user\nsslmode=disable\n");
  write(directory / "env-password", "first:5432:database:user:first_secret\nsecond:5432:database:user:second_secret\n");
  auto loaded = pg::Options::load("", {.environment = true, .user_files = false});
  REQUIRE(loaded);
  REQUIRE(loaded->hosts.size() == 2);
  CHECK(loaded->hosts[0].password == "first_secret");
  CHECK(loaded->hosts[1].password == "second_secret");
  loaded = pg::Options::load("service=system", {.environment = true, .user_files = false});
  REQUIRE(loaded);
  CHECK(loaded->user == "system_user");
}

TEST_CASE("PostgreSQL libpq configuration profile is explicit and discovers conventional credential files")
{
  pg::ConfigSources sources{.environment = false, .user_files = false, .system_files = false};
  auto defaults = pg::Options::load("user=weave", sources);
  REQUIRE(defaults);
  CHECK(defaults->tls_mode == pg::TlsMode::verify_full);
  sources.libpq_compatibility = true;
  auto compatible = pg::Options::load("user=weave", sources);
  REQUIRE(compatible);
  CHECK(compatible->tls_mode == pg::TlsMode::prefer);
  CHECK(compatible->max_protocol == pg::ProtocolVersion::v30);
  REQUIRE(compatible->origin);
  CHECK(compatible->origin->sources.libpq_compatibility);
  auto legacy = pg::Options::load("user=weave requiressl=1", sources);
  REQUIRE(legacy);
  CHECK(legacy->tls_mode == pg::TlsMode::require);
  auto overridden = pg::Options::load(
    "user=weave requiressl=garbage sslmode=verify-full max_protocol_version=latest",
    sources);
  REQUIRE(overridden);
  CHECK(overridden->tls_mode == pg::TlsMode::verify_full);
  CHECK(overridden->max_protocol == pg::ProtocolVersion::v32);
  auto system = pg::Options::load("user=weave sslrootcert=system", sources);
  REQUIRE(system);
  CHECK(system->tls_mode == pg::TlsMode::verify_full);

#ifdef _WIN32
  auto credentials = directory / "postgresql";
#else
  auto credentials = directory / ".postgresql";
#endif
  std::error_code error;
  std::filesystem::create_directory(credentials, error);
  REQUIRE_FALSE(error);
  write(credentials / "root.crt", "fixture-root");
  write(credentials / "postgresql.crt", "fixture-certificate");
  write(credentials / "postgresql.key", "fixture-private-key");
  sources.user_files = true;
  auto discovered = pg::Options::load("user=weave sslmode=require", sources);
  REQUIRE(discovered);
  REQUIRE(discovered->tls_options);
  CHECK(discovered->tls_options->ca_file == path_text(credentials / "root.crt"));
  CHECK(discovered->tls_options->certificate_file == path_text(credentials / "postgresql.crt"));
  CHECK(discovered->tls_options->private_key_file == path_text(credentials / "postgresql.key"));
  CHECK(discovered->tls_options->verification == weave::TlsVerification::certificate);
  auto explicit_root = pg::Options::load("user=weave sslmode=require sslrootcert=explicit.pem", sources);
  REQUIRE(explicit_root);
  CHECK(explicit_root->tls_options->ca_file == "explicit.pem");
  auto disabled = pg::Options::load("user=weave sslmode=disable", sources);
  REQUIRE(disabled);
  CHECK_FALSE(disabled->tls_options);
#ifndef _WIN32
  std::filesystem::permissions(
    credentials / "postgresql.key",
    std::filesystem::perms::others_read,
    std::filesystem::perm_options::add,
    error);
  REQUIRE_FALSE(error);
  auto unsafe = pg::Options::load("user=weave sslmode=require", sources);
  REQUIRE_FALSE(unsafe);
  CHECK(unsafe.error() == std::errc::permission_denied);
#endif
  const std::array files{"root.crt", "postgresql.crt", "postgresql.key"};
  for (auto file : files) {
    CHECK(std::filesystem::remove(credentials / file, error));
    CHECK_FALSE(error);
  }
}

int main(int argc, char **argv)
{
  if (argc < 2)
    return 2;
  directory = std::filesystem::path{argv[1]};
  doctest::Context tests;
  tests.applyCommandLine(argc - 1, argv + 1);
  return tests.run();
}
