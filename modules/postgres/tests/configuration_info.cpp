#include <weave/postgres.hpp>
#include "credential_allocations.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <source_location>
#include <type_traits>
#include <openssl/crypto.h>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static unsigned checks = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Configuration check failed: %u\n", where.line());
    std::_Exit(1);
  }
}

template <class T>
concept Password = requires(T value) { value.password; };
template <class T>
concept PrivatePassword = requires(T value) { value.private_key_password; };
template <class T>
concept ClientKey = requires(T value) { value.scram_client_key; };
template <class T>
concept ServerKey = requires(T value) { value.scram_server_key; };
template <class T>
concept ClientSecret = requires(T value) { value.client_secret; };
static_assert(!Password<pg::OptionsInfo> && !Password<pg::HostInfo>);
static_assert(!PrivatePassword<pg::TlsOptionsInfo>);
static_assert(!ClientKey<pg::OptionsInfo> && !ServerKey<pg::OptionsInfo>);
static_assert(!ClientSecret<pg::OAuthOptionsInfo>);
static_assert(std::is_same_v<decltype(pg::OptionsInfo::tls_context), bool>);
static_assert(std::is_same_v<decltype(pg::OptionsInfo::gss_context), bool>);
static_assert(std::is_same_v<decltype(pg::OAuthOptionsInfo::provider), bool>);
static_assert(std::is_aggregate_v<pg::Options>);

static void tls(const pg::TlsOptionsInfo &info, const weave::TlsClientOptions &options)
{
  check(info.ca_file == options.ca_file);
  check(info.alpn == options.alpn);
  check(info.min_version == options.min_version && info.max_version == options.max_version);
  check(info.certificate_file == options.certificate_file);
  check(info.private_key_file == options.private_key_file);
  check(info.ca_directory == options.ca_directory && info.crl_file == options.crl_file);
  check(info.revocation == options.revocation && info.ocsp == options.ocsp);
  check(info.session_resumption == options.session_resumption);
  check(info.session_lifetime == options.session_lifetime);
  check(info.ocsp_max_age == options.ocsp_max_age && info.ocsp_clock_skew == options.ocsp_clock_skew);
  check(info.limits.buffered_input == options.limits.buffered_input);
  check(info.limits.buffered_output == options.limits.buffered_output);
  check(info.limits.certificate_chain == options.limits.certificate_chain);
  check(info.limits.verification_depth == options.limits.verification_depth);
  check(info.ciphers.tls12 == options.ciphers.tls12 && info.ciphers.tls13 == options.ciphers.tls13);
  check(info.ciphers.groups == options.ciphers.groups && info.ciphers.signatures == options.ciphers.signatures);
  check(info.ciphers.security_level == options.ciphers.security_level);
}

static void sources(const pg::ConfigSources &info, const pg::ConfigSources &options)
{
  check(info.environment == options.environment && info.user_files == options.user_files);
  check(info.system_files == options.system_files && info.ldap == options.ldap);
  check(info.service == options.service && info.service_file == options.service_file);
  check(info.system_service_file == options.system_service_file);
  check(info.password_file == options.password_file && info.ldap_timeout == options.ldap_timeout);
}

static void matches(const pg::OptionsInfo &info, const pg::Options &options)
{
  check(info.host == options.host && info.port == options.port);
  check(info.user == options.user && info.database == (options.database.empty() ? options.user : options.database));
  check(info.application_name == options.application_name && info.client_encoding == options.client_encoding);
  check(info.tls_context == options.tls.has_value());
  check(info.plaintext == options.plaintext);
  check(info.tls_negotiation == options.tls_negotiation);
  check(info.allow_cleartext_password == options.allow_cleartext_password);
  check(info.allow_md5_password == options.allow_md5_password);
  check(info.channel_binding == options.channel_binding && info.connect_timeout == options.connect_timeout);
  check(info.limits.message_bytes == options.limits.message_bytes);
  check(info.limits.result_bytes == options.limits.result_bytes);
  check(info.limits.queued_notifications == options.limits.queued_notifications);
  check(info.limits.scram_iterations == options.limits.scram_iterations);
  check(info.limits.pipeline_commands == options.limits.pipeline_commands);
  check(info.hosts.size() == options.hosts.size());
  for (std::size_t index = 0; index < info.hosts.size(); ++index) {
    check(info.hosts[index].name == options.hosts[index].name);
    check(info.hosts[index].port == options.hosts[index].port);
    check(info.hosts[index].address == options.hosts[index].address);
  }
  check(info.target_session == options.target_session && info.server_options == options.server_options);
  if (options.tls_options) {
    check(info.tls_options.has_value());
    tls(*info.tls_options, *options.tls_options);
  } else if (!options.plaintext && !options.tls) {
    check(info.tls_options.has_value());
    tls(*info.tls_options, weave::TlsClientOptions{});
  } else {
    check(!info.tls_options);
  }
  check(info.settings == options.settings && info.replication == options.replication);
  check(info.keep_alive.enabled == options.keep_alive.enabled);
  check(info.keep_alive.idle == options.keep_alive.idle && info.keep_alive.interval == options.keep_alive.interval);
  check(info.keep_alive.probes == options.keep_alive.probes && info.tcp_user_timeout == options.tcp_user_timeout);
  check(info.host_balance == options.host_balance);
  check(info.authentication.methods == options.authentication.methods);
  check(info.authentication.exclude == options.authentication.exclude);
  check(info.min_protocol == options.min_protocol && info.max_protocol == options.max_protocol);
  check(info.required_peer_user == options.required_peer_user);
  check(info.gss_context == options.gss.has_value() && info.gss_service == options.gss_service);
  check(info.gss_delegation == options.gss_delegation && info.gss_mutual == options.gss_mutual);
  check(info.gss_encryption == options.gss_encryption);
  check(info.oauth.has_value() == options.oauth.has_value());
  if (info.oauth) {
    check(info.oauth->issuer == options.oauth->issuer && info.oauth->client_id == options.oauth->client_id);
    check(info.oauth->scope == options.oauth->scope);
    check(info.oauth->provider == options.oauth->provider.has_value());
    check(info.oauth->acquisition_timeout == options.oauth->acquisition_timeout);
  }
  check(info.origin.has_value() == options.origin.has_value());
  if (info.origin) {
    sources(info.origin->sources, options.origin->sources);
    check(info.origin->service == options.origin->service);
    check(info.origin->service_file == options.origin->service_file);
    check(info.origin->password_file == options.origin->password_file);
  }
}

static weave::Task<pg::OAuthToken> unused_token()
{
  auto token = pg::OAuthToken::parse("unused-fixture-token");
  check(token.has_value());
  co_return std::move(*token);
}

static void direct()
{
  pg::Options defaults;
  auto initial = defaults.info();
  matches(initial, defaults);
  check(initial.user.empty() && initial.database.empty() && !initial.origin);

  pg::Options options;
  options.host = "configured-primary";
  options.port = 6543;
  options.user = "snapshot-owner";
  options.password.assign(127, 'p');
  options.application_name = "configuration-probe";
  options.allow_cleartext_password = true;
  options.allow_md5_password = true;
  options.channel_binding = pg::ChannelBinding::require;
  options.tls_negotiation = pg::TlsNegotiation::direct;
  options.connect_timeout = 12345ms;
  options.limits = {123456, 345678, 42, 12345, 43};
  auto address = weave::IpAddress::parse("::1");
  check(address.has_value());
  options.hosts = {{"first", 6432, *address, std::string(137, 'h')}, {"second", 6433, std::nullopt, "other"}};
  options.target_session = pg::TargetSession::prefer_standby;
  options.server_options = "-c statement_timeout=1234";
  options.tls_options = weave::TlsClientOptions{
    .ca_file = "trust.pem",
    .alpn = {"postgres", "another"},
    .min_version = weave::TlsVersion::tls13,
    .max_version = weave::TlsVersion::tls13,
    .certificate_file = "client.pem",
    .private_key_file = "private.pem",
    .private_key_password = std::string(147, 't'),
    .ca_directory = "trust-directory",
    .crl_file = "revocation.pem",
    .revocation = weave::TlsRevocation::chain,
    .ocsp = weave::TlsOcsp::if_present,
    .session_resumption = true,
    .session_lifetime = 123s,
    .ocsp_max_age = 234s,
    .ocsp_clock_skew = 12s,
    .limits = {123456, 234567, 34567, 7},
    .ciphers = {"tls12", "tls13", "groups", "signatures", 3}};
  options.client_encoding = "LATIN1";
  options.settings = {{"a", "one"}, {"b", "two"}};
  options.replication = pg::Replication::database;
  options.keep_alive = {.enabled = false, .idle = 23s, .interval = 34s, .probes = 5};
  options.tcp_user_timeout = 23456ms;
  options.host_balance = pg::HostBalance::random;
  options.authentication = {{pg::Authentication::password, pg::Authentication::md5}, true};
  options.min_protocol = pg::ProtocolVersion::v32;
  options.max_protocol = pg::ProtocolVersion::v32;
  options.required_peer_user = 12345;
  std::array<std::byte, 32> key;
  key.fill(std::byte{17});
  options.scram_client_key.emplace(key);
  key.fill(std::byte{23});
  options.scram_server_key.emplace(key);
  options.gss_service = "other-service";
  options.gss_delegation = true;
  options.gss_mutual = false;
  options.gss_encryption = pg::GssEncryption::require;
  auto client_secret = pg::OAuthClientSecret::parse(std::string(157, 's'));
  check(client_secret.has_value());
  auto owner = std::make_shared<unsigned>(0);
  std::weak_ptr<unsigned> retained = owner;
  auto provider = pg::OAuthProvider::create([owner](pg::OAuthRequest) noexcept {
    ++*owner;
    return unused_token();
  });
  check(provider.has_value());
  options.oauth = pg::OAuthOptions{
    .issuer = "https://issuer.example",
    .client_id = "probe-client",
    .scope = "scope-one scope-two",
    .client_secret = std::move(*client_secret),
    .provider = std::move(*provider),
    .acquisition_timeout = 34567ms};
  options.origin = pg::ConfigurationOrigin{
    .sources =
      {.environment = false,
        .user_files = false,
        .service = "source-service",
        .service_file = "user-source",
        .system_service_file = "system-source",
        .password_file = "password-source",
        .ldap = false,
        .ldap_timeout = 4321ms,
        .system_files = false},
    .service = "selected-service",
    .service_file = "selected-file",
    .password_file = "selected-password-file"};

  fixture::CredentialAllocation password(options.password);
  fixture::CredentialAllocation host_password(*options.hosts[0].password);
  fixture::CredentialAllocation tls_password(options.tls_options->private_key_password);
  auto info = options.info();
  matches(info, options);
  check(info.database == "snapshot-owner" && *owner == 0);
  check(!password.released && !host_password.released && !tls_password.released);
  auto copied = info;
  options.hosts[0].name = "changed";
  options.tls_options->alpn.clear();
  options.settings.clear();
  options.tls_negotiation = pg::TlsNegotiation::postgres;
  options.origin->service_file = "changed";
  options.oauth.reset();
  owner.reset();
  check(retained.expired());
  check(copied.hosts[0].name == "first" && info.hosts[0].name == "first");
  check(copied.tls_options->alpn.size() == 2 && info.settings.size() == 2);
  check(copied.tls_negotiation == pg::TlsNegotiation::direct && info.tls_negotiation == pg::TlsNegotiation::direct);
  check(info.origin->service_file == "selected-file" && info.oauth->provider);

  defaults.user = "database-fallback";
  check(defaults.info().database == "database-fallback");
  defaults.database = "explicit-database";
  check(defaults.info().database == "explicit-database");
  defaults.plaintext = true;
  matches(defaults.info(), defaults);
  defaults.plaintext = false;
  auto context = weave::TlsContext::client();
  check(context.has_value());
  defaults.tls = std::move(*context);
  auto opaque = defaults.info();
  matches(opaque, defaults);
  check(opaque.tls_context && !opaque.tls_options);
  defaults = {};
  check(opaque.tls_context && !opaque.tls_options);
}

static void loaded(std::string_view mode, const std::filesystem::path &root)
{
  pg::ConfigSources configuration{.environment = false, .user_files = false, .ldap = false, .system_files = false};
  if (mode == "builtin") {
    auto defaults = pg::Options::defaults(configuration);
    check(defaults.has_value());
    check(defaults->host == "localhost" && defaults->port == 5432);
    check(!defaults->user.empty() && defaults->database == defaults->user);
    check(defaults->application_name == "weave" && defaults->client_encoding == "UTF8");
    check(defaults->tls_options.has_value() && !defaults->tls_context);
    check(defaults->origin && !defaults->origin->service && !defaults->origin->service_file);
    check(!defaults->origin->password_file);
    sources(defaults->origin->sources, configuration);
    return;
  }
  configuration.service = "selected";
  configuration.service_file = (root / "user.conf").string();
  configuration.system_service_file = (root / "system.conf").string();
  configuration.password_file = (root / "passwords").string();
  configuration.system_files = true;
  if (mode == "environment") {
    configuration.environment = true;
    configuration.service.clear();
    configuration.service_file.clear();
    configuration.password_file.clear();
  }
  if (mode == "missing")
    configuration.service = "missing";

  std::string pattern(177, 'q');
  fixture::CredentialPattern credentials(pattern);
  auto defaults = pg::Options::defaults(configuration);
  if (mode == "missing") {
    check(!defaults && defaults.error() == std::errc::no_such_file_or_directory);
    return;
  }
  check(defaults.has_value());
  check(defaults->user == "snapshot-user" && defaults->database == "snapshot-database");
  check(defaults->port == 6543 && defaults->application_name == "snapshot-app");
  check(defaults->server_options == "-c work_mem=4096" && defaults->hosts.size() == 1);
  check(defaults->hosts[0].name == "snapshot-host" && defaults->hosts[0].port == 6543);
  check(defaults->connect_timeout == 7000ms && defaults->plaintext);
  check(defaults->origin && defaults->origin->service == "selected");
  auto selected = mode == "system" ? "system.conf" : "user.conf";
  check(defaults->origin->service_file == (root / selected).string());
  check(defaults->origin->password_file == (root / "passwords").string());
  sources(defaults->origin->sources, configuration);
  check(credentials.dirty_releases() == 0);
  check(fixture::credential_copy(pattern).empty());
  {
    auto options = pg::Options::load({}, configuration);
    check(options.has_value());
    bool file_password = options->hosts[0].password == pattern;
    bool environment_password = options->password == pattern;
    check(file_password || environment_password);
    auto unstarted = pg::connect(std::move(*options));
  }
  check(credentials.dirty_releases() == 0 && fixture::credential_copy(pattern).empty());
  auto copy = *defaults;
  defaults = std::unexpected(std::make_error_code(std::errc::operation_canceled));
  check(copy.user == "snapshot-user" && copy.origin->service == "selected");
}

int main(int argc, char **argv)
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  if (argc != 3)
    return 1;
  std::string_view mode = argv[1];
  if (mode == "direct")
    direct();
  else
    loaded(mode, argv[2]);
  std::printf("Configuration values passed: %u checks; OpenSSL: %s\n", checks, OpenSSL_version(OPENSSL_VERSION));
}
