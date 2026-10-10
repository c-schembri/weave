#include <weave/postgres/connection.hpp>
#include "options.hpp"

namespace weave::pg {

namespace {

TlsOptionsInfo tls_info(const TlsClientOptions &options)
{
  TlsOptionsInfo info;
  info.ca_file = options.ca_file;
  info.alpn = options.alpn;
  info.min_version = options.min_version;
  info.max_version = options.max_version;
  info.certificate_file = options.certificate_file;
  // Provider URIs can embed PINs; a nonsecret snapshot must never publish them.
  if (options.private_key_format != TlsPrivateKeyFormat::store)
    info.private_key_file = options.private_key_file;
  info.ca_directory = options.ca_directory;
  info.crl_file = options.crl_file;
  info.crl_directory = options.crl_directory;
  info.revocation = options.revocation;
  info.ocsp = options.ocsp;
  info.session_resumption = options.session_resumption;
  info.session_lifetime = options.session_lifetime;
  info.ocsp_max_age = options.ocsp_max_age;
  info.ocsp_clock_skew = options.ocsp_clock_skew;
  info.limits = options.limits;
  info.ciphers = options.ciphers;
  info.private_key_password_provider = options.private_key_password_provider.has_value();
  info.private_key_format = options.private_key_format;
  info.verification = options.verification;
  info.key_logging = !options.key_log_file.empty();
  return info;
}

} // namespace

OptionsInfo Options::info() const
{
  // Copy an explicit nonsecret whitelist, never a credential-bearing Options/provider object.
  OptionsInfo info;
  info.host = host;
  info.port = port;
  info.user = user;
  info.database = database.empty() ? user : database;
  info.application_name = application_name;
  info.tls_context = tls.has_value();
  info.plaintext = plaintext || tls_mode == TlsMode::disable;
  info.tls_mode = plaintext ? TlsMode::disable : tls_mode;
  info.allow_cleartext_password = allow_cleartext_password;
  info.allow_md5_password = allow_md5_password;
  info.channel_binding = channel_binding;
  info.connect_timeout = connect_timeout;
  info.limits = limits;
  info.hosts.reserve(hosts.size());
  for (const auto &destination : hosts)
    info.hosts.push_back({destination.name, destination.port, destination.address});
  info.target_session = target_session;
  info.server_options = server_options;
  if (tls_options)
    info.tls_options = tls_info(*tls_options);
  else if (!info.plaintext && !tls)
    info.tls_options = tls_info(TlsClientOptions{});
  if (info.tls_options && info.tls_mode != TlsMode::verify_full) {
    auto &policy = *info.tls_options;
    policy.verification = info.tls_mode == TlsMode::verify_ca || !policy.ca_file.empty() || !policy.ca_directory.empty()
      ? TlsVerification::certificate
      : TlsVerification::none;
  }
  info.client_encoding = client_encoding;
  info.settings = settings;
  info.replication = replication;
  info.keep_alive = keep_alive;
  info.tcp_user_timeout = tcp_user_timeout;
  info.host_balance = host_balance;
  info.authentication = authentication;
  info.min_protocol = min_protocol;
  info.max_protocol = max_protocol;
  info.required_peer_user = required_peer_user;
  info.gss_context = gss.has_value();
  info.gss_service = gss_service;
  info.gss_delegation = gss_delegation;
  info.gss_mutual = gss_mutual;
  info.gss_encryption = gss_encryption;
  if (oauth) {
    info.oauth = OAuthOptionsInfo{
      oauth->issuer,
      oauth->client_id,
      oauth->scope,
      oauth->provider.has_value(),
      oauth->acquisition_timeout};
  }
  info.origin = origin;
  info.tls_negotiation = tls_negotiation;
  info.server_name_indication = server_name_indication;
  info.client_certificate = client_certificate;
  return info;
}

Result<OptionsInfo> Options::defaults(ConfigSources sources)
{
  auto options = load({}, std::move(sources));
  if (!options)
    return std::unexpected(options.error());

  detail::OptionsCleanup cleanup{*options};
  return options->info();
}

} // namespace weave::pg
