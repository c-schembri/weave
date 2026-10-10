#include <weave/postgres/configuration.hpp>
#include <weave/postgres/connection.hpp>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

namespace pg = weave::pg;
static unsigned checks = 0;

static void check(bool value)
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Failed check %u\n", checks);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
  }
}

static const pg::OptionDescriptor &find(std::string_view keyword)
{
  auto schema = pg::option_schema();
  auto found = std::ranges::find(schema, keyword, &pg::OptionDescriptor::keyword);
  check(found != schema.end());
  return *found;
}

static std::string value(std::string_view key)
{
  if (key == "hostaddr")
    return "127.0.0.1";
  if (key == "scram_client_key" || key == "scram_server_key")
    return "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
  if (key == "require_auth")
    return "scram-sha-256";
  if (key == "oauth_issuer")
    return "https://issuer.example";
  if (key == "oauth_client_secret")
    return "private-test-secret";
  if (key == "gsslib") {
#ifdef _WIN32
    return "sspi";
#else
    return "gssapi";
#endif
  }
  const auto &option = find(key);
  return option.default_value ? std::string(*option.default_value) : "sample";
}

static void environment(std::string_view key)
{
  auto parsed = pg::Options::load({}, {.environment = true, .user_files = false, .system_files = false});
  const auto &option = find(key);
  bool unsupported = option.support == pg::OptionSupport::unsupported && key != "servicefile";
#ifdef _WIN32
  unsupported = unsupported || key == "requirepeer";
#endif
  if (unsupported) {
    check(!parsed);
    check(parsed.error() == std::errc::operation_not_supported);
    return;
  }
  check(parsed.has_value());
  auto &options = *parsed;
  if (key == "host")
    check(options.host == "schema-host");
  else if (key == "hostaddr")
    check(
      options.hosts.size() == 1 && options.hosts.front().address &&
      options.hosts.front().address->to_string() == "127.0.0.2");
  else if (key == "port")
    check(options.port == 5437);
  else if (key == "user")
    check(options.user == "schema-user");
  else if (key == "dbname")
    check(options.database == "schema-db");
  else if (key == "password")
    check(options.password == "schema-password");
  else if (key == "application_name")
    check(options.application_name == "schema-app");
  else if (key == "options")
    check(options.server_options == "-csearch_path=");
  else if (key == "client_encoding")
    check(options.client_encoding == "LATIN1");
  else if (key == "connect_timeout")
    check(options.connect_timeout == std::chrono::seconds{17});
  else if (key == "channel_binding")
    check(options.channel_binding == pg::ChannelBinding::disable);
  else if (key == "target_session_attrs")
    check(options.target_session == pg::TargetSession::read_write);
  else if (key == "require_auth")
    check(options.authentication.methods == std::vector{pg::Authentication::scram_sha256});
  else if (key == "min_protocol_version")
    check(options.min_protocol == pg::ProtocolVersion::v32);
  else if (key == "max_protocol_version")
    check(options.max_protocol == pg::ProtocolVersion::v30);
  else if (key == "sslmode")
    check(options.plaintext);
  else if (key == "sslnegotiation")
    check(options.tls_negotiation == pg::TlsNegotiation::direct && !options.plaintext);
  else if (key == "sslrootcert")
    check(options.tls_options && options.tls_options->ca_file == "schema-root");
  else if (key == "sslcert")
    check(options.tls_options && options.tls_options->certificate_file == "schema-cert");
  else if (key == "sslkey")
    check(options.tls_options && options.tls_options->private_key_file == "schema-key");
  else if (key == "sslcrl")
    check(
      options.tls_options && options.tls_options->crl_file == "schema-crl" &&
      options.tls_options->revocation == weave::TlsRevocation::chain);
  else if (key == "sslcrldir")
    check(
      options.tls_options && options.tls_options->crl_directory == "schema-crldir" &&
      options.tls_options->revocation == weave::TlsRevocation::chain);
  else if (key == "ssl_min_protocol_version")
    check(options.tls_options && options.tls_options->min_version == weave::TlsVersion::tls13);
  else if (key == "ssl_max_protocol_version")
    check(options.tls_options && options.tls_options->max_version == weave::TlsVersion::tls12);
  else if (key == "gssencmode")
    check(options.gss_encryption == pg::GssEncryption::require);
  else if (key == "krbsrvname")
    check(options.gss_service == "schema-service");
  else if (key == "gsslib")
    check(!options.gss);
  else if (key == "gssdelegation")
    check(options.gss_delegation);
  else if (key == "load_balance_hosts")
    check(options.host_balance == pg::HostBalance::random);
  else if (key == "service" || key == "servicefile")
    check(options.user == "service-user");
  else if (key == "sslsni")
    check(!options.server_name_indication && !options.info().server_name_indication);
  else if (key == "sslcertmode")
    check(
      options.client_certificate == weave::TlsCertificateMode::require &&
      options.info().client_certificate == weave::TlsCertificateMode::require);
  else if (key == "passfile")
    check(options.hosts.empty() && options.password == "file-password");
  else if (key == "requirepeer")
    check(options.required_peer_user.has_value());
  else
    check(false);
}

int main(int argc, char **argv)
{
  static_assert(noexcept(pg::option_schema()));
  static_assert(std::same_as<decltype(pg::option_schema()), std::span<const pg::OptionDescriptor>>);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  if (argc == 2) {
    environment(argv[1]);
    std::printf("environment\t%s\t%u\n", argv[1], checks);
    return 0;
  }
  check(argc == 1);
  auto schema = pg::option_schema();
  check(!schema.empty());
  check(schema.data() == pg::option_schema().data());

  for (std::size_t index = 0; index < schema.size(); ++index) {
    const auto &option = schema[index];
    check(!option.keyword.empty());
    check(!option.constraints.empty());
    auto previous = schema.first(index);
    check(std::ranges::find(previous, option.keyword, &pg::OptionDescriptor::keyword) == previous.end());
    if (!option.environment.empty())
      check(std::ranges::find(previous, option.environment, &pg::OptionDescriptor::environment) == previous.end());

    auto text = std::string(option.keyword) + "='" + value(option.keyword) + "'";
    auto parsed = pg::Options::parse(text);
    if (option.support == pg::OptionSupport::parse) {
      check(parsed.has_value());
    } else {
      check(!parsed);
      auto expected = option.support == pg::OptionSupport::unrecognized ? std::errc::invalid_argument
                                                                        : std::errc::operation_not_supported;
      check(parsed.error() == expected);
    }
  }

  pg::Options defaults;
  weave::TlsClientOptions tls;
  const std::array expected{
    std::pair{"host", defaults.host},
    std::pair{"port", std::to_string(defaults.port)},
    std::pair{"user", defaults.user},
    std::pair{"dbname", defaults.database},
    std::pair{"password", defaults.password},
    std::pair{"application_name", defaults.application_name},
    std::pair{"options", defaults.server_options},
    std::pair{"client_encoding", defaults.client_encoding},
    std::pair{"connect_timeout", std::to_string(defaults.connect_timeout.count() / 1000)},
    std::pair{"keepalives", defaults.keep_alive.enabled ? std::string{"1"} : std::string{"0"}},
    std::pair{"keepalives_idle", std::to_string(defaults.keep_alive.idle.count())},
    std::pair{"keepalives_interval", std::to_string(defaults.keep_alive.interval.count())},
    std::pair{"keepalives_count", std::to_string(defaults.keep_alive.probes)},
    std::pair{"tcp_user_timeout", std::to_string(defaults.tcp_user_timeout.count())}};
  for (const auto &[key, text] : expected)
    check(find(key).default_value == text);

  check(find("channel_binding").default_value == "prefer" && defaults.channel_binding == pg::ChannelBinding::prefer);
  check(find("replication").default_value == "false" && defaults.replication == pg::Replication::disabled);
  check(find("target_session_attrs").default_value == "any" && defaults.target_session == pg::TargetSession::any);
  check(find("sslmode").default_value == "verify-full" && !defaults.plaintext);
  check(find("sslnegotiation").default_value == "postgres" && defaults.tls_negotiation == pg::TlsNegotiation::postgres);
  check(find("sslrootcert").default_value == "system" && tls.ca_file.empty() && tls.ca_directory.empty());
  check(find("ssl_min_protocol_version").default_value == "TLSv1.2" && tls.min_version == weave::TlsVersion::tls12);
  check(find("ssl_max_protocol_version").default_value == "TLSv1.3" && tls.max_version == weave::TlsVersion::tls13);
  check(find("gssencmode").default_value == "disable" && defaults.gss_encryption == pg::GssEncryption::disable);
  check(find("krbsrvname").default_value == defaults.gss_service);
  check(find("gssdelegation").default_value == "0" && !defaults.gss_delegation);
  check(find("load_balance_hosts").default_value == "disable" && defaults.host_balance == pg::HostBalance::ordered);
  check(find("min_protocol_version").default_value == "3.0" && defaults.min_protocol == pg::ProtocolVersion::v30);
  check(find("max_protocol_version").default_value == "3.2" && defaults.max_protocol == pg::ProtocolVersion::v32);

  std::printf("checks\t%u\nopenssl\t%lu\n", checks, OpenSSL_version_num());
  for (const auto &option : schema) {
    auto keyword = std::string(option.keyword);
    auto environment = std::string(option.environment);
    auto text = option.default_value ? std::string(*option.default_value) : "\\N";
    std::printf(
      "option\t%s\t%s\t%s\t%d\t%d\n",
      keyword.c_str(),
      environment.c_str(),
      text.c_str(),
      static_cast<int>(option.support),
      option.secret);
  }
}
