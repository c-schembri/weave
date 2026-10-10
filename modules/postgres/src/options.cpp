#include <weave/postgres/connection.hpp>
#include "options.hpp"
#include "gss.hpp"
#include "secret.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <map>

namespace weave::pg {

namespace {

constexpr auto maximum_input = detail::maximum_connection_input;
constexpr auto maximum_field = detail::maximum_connection_field;
constexpr std::size_t maximum_hosts = 64;
using Fields = detail::OptionFields;

void clear_moved_text(std::string &text, char *original_data, std::size_t original_size) noexcept
{
  // Re-expose retained inline characters through the string API, not writes past size().
  if (text.data() == original_data && text.size() < original_size && original_size <= text.capacity())
    text.resize(original_size);
  OPENSSL_cleanse(text.data(), text.size());
  text.clear();
}

struct TextCleanup {
  std::string &text;
  char *data = text.data();
  std::size_t size = text.size();

  ~TextCleanup()
  {
    clear_moved_text(text, data, size);
  }
};

std::error_code invalid()
{
  return std::make_error_code(std::errc::invalid_argument);
}

std::error_code unsupported()
{
  return std::make_error_code(std::errc::operation_not_supported);
}

bool whitespace(char value)
{
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f' || value == '\v';
}

bool assignment_space(char value, bool service)
{
  return service ? (value == ' ' || value == '\t') : whitespace(value);
}

struct TlsPasswordCleanup {
  TlsClientOptions &options;

  ~TlsPasswordCleanup()
  {
    OPENSSL_cleanse(options.private_key_password.data(), options.private_key_password.size());
  }
};

int hex(char digit)
{
  if (digit >= '0' && digit <= '9')
    return digit - '0';
  if (digit >= 'a' && digit <= 'f')
    return digit - 'a' + 10;
  if (digit >= 'A' && digit <= 'F')
    return digit - 'A' + 10;
  return -1;
}

Result<detail::SecretText> decode_bytes(std::string_view text)
{
  detail::SecretText value;
  value.reserve(std::min(text.size(), maximum_field));
  for (std::size_t index = 0; index < text.size(); ++index) {
    char byte = text[index];
    if (byte == '%') {
      if (text.size() - index < 3)
        return std::unexpected(invalid());
      int high = hex(text[index + 1]);
      int low = hex(text[index + 2]);
      if (high < 0 || low < 0)
        return std::unexpected(invalid());
      byte = static_cast<char>((high << 4) | low);
      index += 2;
    }
    if (byte == '\0' || value.size() == maximum_field)
      return std::unexpected(invalid());
    value.push_back(byte);
  }
  return value;
}

Result<std::string> decode(std::string_view text)
{
  auto value = decode_bytes(text);
  if (!value)
    return std::unexpected(value.error());
  return std::string{value->data(), value->size()};
}

Result<void> decoded_field(Fields &fields, std::string_view key, std::string_view value)
{
  auto decoded = decode_bytes(value);
  if (!decoded)
    return std::unexpected(decoded.error());
  return fields.put(key, std::string{decoded->data(), decoded->size()});
}

Result<void> keywords(Fields &fields, std::string_view input, bool service = false)
{
  std::size_t index = 0;
  while (index < input.size()) {
    while (index < input.size() && whitespace(input[index]))
      ++index;
    if (index == input.size())
      break;

    auto start = index;
    while (index < input.size() && !whitespace(input[index]) && input[index] != '=')
      ++index;
    auto key = input.substr(start, index - start);
    while (index < input.size() && assignment_space(input[index], service))
      ++index;
    if (key.empty() || index == input.size() || input[index++] != '=')
      return std::unexpected(invalid());
    while (index < input.size() && assignment_space(input[index], service))
      ++index;

    detail::SecretText value;
    bool quoted = index < input.size() && input[index] == '\'';
    bool closed = !quoted;
    if (quoted)
      ++index;
    while (index < input.size()) {
      char byte = input[index++];
      if (quoted && byte == '\'') {
        closed = true;
        break;
      }
      if (!quoted && whitespace(byte))
        break;
      if (byte == '\\' && (!service || quoted)) {
        if (index == input.size())
          return std::unexpected(invalid());
        byte = input[index++];
      }
      if (value.size() == maximum_field)
        return std::unexpected(invalid());
      value.push_back(byte);
    }
    if (!closed || (quoted && index < input.size() && !whitespace(input[index])))
      return std::unexpected(invalid());
    if (!service || !fields.get(key)) {
      if (auto result = fields.put(key, std::string{value.data(), value.size()}); !result)
        return result;
    }
  }
  return {};
}

std::vector<std::string_view> split(std::string_view text, char delimiter)
{
  std::vector<std::string_view> parts;
  for (;;) {
    auto end = text.find(delimiter);
    parts.push_back(text.substr(0, end));
    if (end == std::string_view::npos)
      return parts;
    text.remove_prefix(end + 1);
  }
}

Result<void> uri(Fields &fields, std::string_view input)
{
  if (input.find('#') != std::string_view::npos || std::ranges::any_of(input, whitespace))
    return std::unexpected(invalid());

  auto query_start = input.find('?');
  auto path = input.substr(0, query_start);
  auto slash = path.find('/');
  auto authority = path.substr(0, slash);
  auto at = authority.find('@');
  if (at != std::string_view::npos) {
    if (authority.find('@', at + 1) != std::string_view::npos)
      return std::unexpected(invalid());
    auto identity = authority.substr(0, at);
    auto colon = identity.find(':');
    if (auto result = decoded_field(fields, "user", identity.substr(0, colon)); !result)
      return result;
    if (colon != std::string_view::npos) {
      if (auto result = decoded_field(fields, "password", identity.substr(colon + 1)); !result)
        return result;
    }
    authority.remove_prefix(at + 1);
  }

  if (!authority.empty()) {
    auto endpoints = split(authority, ',');
    if (endpoints.size() > maximum_hosts)
      return std::unexpected(invalid());

    std::string names;
    std::string ports;
    bool first = true;
    for (auto endpoint : endpoints) {
      std::string_view name;
      std::string_view port;
      if (endpoint.starts_with('[')) {
        auto close = endpoint.find(']');
        if (close == std::string_view::npos || close == 1)
          return std::unexpected(invalid());
        name = endpoint.substr(1, close - 1);
        auto suffix = endpoint.substr(close + 1);
        if (!suffix.empty()) {
          if (!suffix.starts_with(':'))
            return std::unexpected(invalid());
          port = suffix.substr(1);
        }
        auto decoded = decode(name);
        if (!decoded)
          return std::unexpected(decoded.error());
        auto address = IpAddress::parse(*decoded);
        if (!address || address->is_v4())
          return std::unexpected(invalid());
      } else {
        auto colon = endpoint.find(':');
        name = endpoint.substr(0, colon);
        if (colon != std::string_view::npos) {
          port = endpoint.substr(colon + 1);
          if (port.find(':') != std::string_view::npos)
            return std::unexpected(invalid());
        }
        if (name.find_first_of("[]") != std::string_view::npos)
          return std::unexpected(invalid());
      }

      auto decoded_name = decode(name);
      auto decoded_port = decode(port);
      if (!decoded_name || !decoded_port)
        return std::unexpected(invalid());
      if (!first) {
        names.push_back(',');
        ports.push_back(',');
      }
      first = false;
      names += *decoded_name;
      ports += *decoded_port;
    }
    if (auto result = fields.put("host", std::move(names)); !result)
      return result;
    if (auto result = fields.put("port", std::move(ports)); !result)
      return result;
  }

  if (slash != std::string_view::npos) {
    auto database = path.substr(slash + 1);
    if (database.find('/') != std::string_view::npos)
      return std::unexpected(invalid());
    if (auto result = decoded_field(fields, "dbname", database); !result)
      return result;
  }

  if (query_start != std::string_view::npos) {
    auto query = input.substr(query_start + 1);
    if (!query.empty()) {
      for (auto parameter : split(query, '&')) {
        auto equals = parameter.find('=');
        if (equals == std::string_view::npos)
          return std::unexpected(invalid());
        auto key = decode(parameter.substr(0, equals));
        if (!key)
          return std::unexpected(key.error());
        if (auto result = decoded_field(fields, *key, parameter.substr(equals + 1)); !result)
          return result;
      }
    }
  }
  return {};
}

template <class T>
Result<T> number(std::string_view input)
{
  T value{};
  auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size())
    return std::unexpected(invalid());
  return value;
}

Result<void> hosts(Options &options, const Fields &fields)
{
  auto names = fields.get("host");
  auto addresses = fields.get("hostaddr");
  auto ports = fields.get("port");
  if (!names && !addresses && !ports)
    return {};

  auto name_list = split(names ? std::string_view{*names} : std::string_view{}, ',');
  auto address_list = split(addresses ? std::string_view{*addresses} : std::string_view{}, ',');
  auto port_list = split(ports ? std::string_view{*ports} : std::string_view{}, ',');
  auto count = std::max(name_list.size(), address_list.size());
  bool mismatched = (names && name_list.size() != count) || (addresses && address_list.size() != count) ||
    (port_list.size() != 1 && port_list.size() != count);
  if (count > maximum_hosts || mismatched)
    return std::unexpected(invalid());

  for (std::size_t index = 0; index < count; ++index) {
    Host host;
    auto name = names ? name_list[index] : std::string_view{};
    auto address = addresses ? address_list[index] : std::string_view{};
    if (detail::local_host(name) && !address.empty())
      return std::unexpected(invalid());
    if (!name.empty())
      host.name = name;
    if (!address.empty()) {
      auto parsed = IpAddress::parse(address);
      if (!parsed)
        return std::unexpected(invalid());
      host.address = *parsed;
      if (name.empty())
        host.name = address;
    }
    auto port = port_list[port_list.size() == 1 ? 0 : index];
    if (!port.empty()) {
      auto parsed = number<u16>(port);
      if (!parsed || *parsed == 0)
        return std::unexpected(invalid());
      host.port = *parsed;
    }
    if (detail::local_host(host.name)) {
      if (auto path = detail::local_socket_path(host.name, host.port); !path)
        return std::unexpected(path.error());
    }
    options.hosts.push_back(std::move(host));
  }
  options.host = options.hosts.front().name;
  options.port = options.hosts.front().port;
  return {};
}

Result<TlsVersion> tls_version(std::string_view value)
{
  if (value == "TLSv1.2")
    return TlsVersion::tls12;
  if (value == "TLSv1.3")
    return TlsVersion::tls13;
  return std::unexpected(unsupported());
}

Result<ProtocolVersion> protocol_version(std::string_view value)
{
  if (value == "3.0")
    return ProtocolVersion::v30;
  if (value == "3.2" || value == "latest")
    return ProtocolVersion::v32;
  return std::unexpected(invalid());
}

Result<AuthenticationPolicy> authentication_policy(std::string_view value)
{
  AuthenticationPolicy policy;
  const std::array labels{"none", "password", "md5", "scram-sha-256", "gss", "sspi", "oauth"};
  auto methods = split(value, ',');
  if (methods.size() > 64)
    return std::unexpected(invalid());
  policy.exclude = value.starts_with('!');
  for (auto method : methods) {
    bool excluded = method.starts_with('!');
    if (excluded != policy.exclude)
      return std::unexpected(invalid());
    if (excluded)
      method.remove_prefix(1);
    auto found = std::ranges::find(labels, method);
    if (found == labels.end())
      return std::unexpected(invalid());
    auto selected = static_cast<Authentication>(found - labels.begin());
    if (std::ranges::find(policy.methods, selected) == policy.methods.end())
      policy.methods.push_back(selected);
  }
  if (auto valid = detail::valid_authentication_policy(policy); !valid)
    return std::unexpected(valid.error());
  return policy;
}

using Support = OptionSupport;

constexpr OptionDescriptor schema[]{
  {"host", "PGHOST", "localhost", Support::parse, false, "At most 64 host entries; local paths select local sockets"},
  {"hostaddr", "PGHOSTADDR", {}, Support::parse, false, "Numeric addresses; host-list lengths must agree"},
  {"port", "PGPORT", "5432", Support::parse, false, "1-65535; one port or one per host"},
  {"user", "PGUSER", "", Support::parse, false, "load resolves an absent/empty user from the OS identity"},
  {"dbname", "PGDATABASE", "", Support::parse, false, "An empty database resolves to the selected user at startup"},
  {"password", "PGPASSWORD", "", Support::parse, true, "Never retained for session inspection"},
  {"scram_client_key", "", {}, Support::parse, true, "Base64-encoded 32-byte key; authentication policy still applies"},
  {"scram_server_key", "", {}, Support::parse, true, "Base64-encoded 32-byte key; server proof is mandatory"},
  {"application_name",
    "PGAPPNAME",
    "weave",
    Support::parse,
    false,
    "Nonempty values override fallback_application_name"},
  {"fallback_application_name", "", {}, Support::parse, false, "Used when application_name is absent or empty"},
  {"options", "PGOPTIONS", "", Support::parse, false, "Startup request text, not sanitized text or current GUCs"},
  {"client_encoding", "PGCLIENTENCODING", "UTF8", Support::parse, false, "auto is unsupported"},
  {"connect_timeout",
    "PGCONNECT_TIMEOUT",
    "30",
    Support::parse,
    false,
    "1-86400 seconds; unlike libpq, zero is invalid"},
  {"channel_binding", "PGCHANNELBINDING", "prefer", Support::parse, false, "disable, prefer or require"},
  {"target_session_attrs",
    "PGTARGETSESSIONATTRS",
    "any",
    Support::parse,
    false,
    "Includes primary/standby and read-write/read-only policies"},
  {"replication", "", "false", Support::parse, false, "Boolean physical mode or database mode"},
  {"keepalives", "", "1", Support::parse, false, "Nonnegative integer; zero disables TCP keepalive"},
  {"keepalives_idle", "", "0", Support::parse, false, "Nonnegative seconds; zero preserves the OS default"},
  {"keepalives_interval", "", "0", Support::parse, false, "Nonnegative seconds; zero preserves the OS default"},
  {"keepalives_count", "", "0", Support::parse, false, "Nonnegative probes; zero preserves the OS default"},
  {"tcp_user_timeout", "", "0", Support::parse, false, "Nonnegative milliseconds; Windows accepts only zero"},
  {"require_auth",
    "PGREQUIREAUTH",
    {},
    Support::parse,
    false,
    "Allowed/excluded methods; does not opt in to weak passwords"},
  {"oauth_issuer", "", {}, Support::parse, false, "Explicit OAuth provider configuration"},
  {"oauth_client_id", "", {}, Support::parse, false, "Explicit OAuth provider configuration"},
  {"oauth_scope", "", {}, Support::parse, false, "Explicit OAuth provider configuration"},
  {"oauth_client_secret", "", {}, Support::parse, true, "Owning parsed secret; never exported by session inspection"},
  {"min_protocol_version",
    "PGMINPROTOCOLVERSION",
    "3.0",
    Support::parse,
    false,
    "3.0, 3.2 or latest; minimum must not exceed maximum"},
  {"max_protocol_version",
    "PGMAXPROTOCOLVERSION",
    "3.2",
    Support::parse,
    false,
    "3.0, 3.2 or latest; minimum must not exceed maximum"},
  {"sslmode",
    "PGSSLMODE",
    "verify-full",
    Support::parse,
    false,
    "disable, allow, prefer, require, verify-ca or verify-full; default remains verify-full"},
  {"sslrootcert",
    "PGSSLROOTCERT",
    "system",
    Support::parse,
    false,
    "System trust or explicit CA file; not libpq default-file discovery"},
  {"sslcert", "PGSSLCERT", {}, Support::parse, false, "Explicit client certificate file"},
  {"sslkey", "PGSSLKEY", {}, Support::parse, false, "Explicit private-key file; not engine/provider URI selection"},
  {"sslpassword", "", {}, Support::parse, true, "Explicit private-key passphrase; never prompts on stdin"},
  {"sslcrl", "PGSSLCRL", {}, Support::parse, false, "Explicit CRL file enables chain revocation checking"},
  {"ssl_min_protocol_version", "PGSSLMINPROTOCOLVERSION", "TLSv1.2", Support::parse, false, "TLSv1.2 or TLSv1.3"},
  {"ssl_max_protocol_version", "PGSSLMAXPROTOCOLVERSION", "TLSv1.3", Support::parse, false, "TLSv1.2 or TLSv1.3"},
  {"gssencmode",
    "PGGSSENCMODE",
    "disable",
    Support::parse,
    false,
    "disable, prefer or require; provider availability still applies"},
  {"krbsrvname", "PGKRBSRVNAME", "postgres", Support::parse, false, "Validated GSS service name"},
  {"gsslib",
    "PGGSSLIB",
    {},
    Support::parse,
    false,
    "sspi on Windows, gssapi on Linux; opposite provider is unsupported"},
  {"gssdelegation", "PGGSSDELEGATION", "0", Support::parse, false, "0 or 1; explicit captured provider identity"},
  {"load_balance_hosts", "PGLOADBALANCEHOSTS", "disable", Support::parse, false, "disable or random"},
  {"service",
    "PGSERVICE",
    {},
    Support::load,
    false,
    "Explicit loader/service selection; nested service references are rejected"},
  {"servicefile",
    "PGSERVICEFILE",
    {},
    Support::unsupported,
    false,
    "Use ConfigSources.service_file or PGSERVICEFILE, not this keyword"},
  {"passfile",
    "PGPASSFILE",
    {},
    Support::load,
    false,
    "Loader only; file permissions and host-specific selection apply"},
  {"sslcompression", "PGSSLCOMPRESSION", {}, Support::unsupported, false, "TLS compression is not exposed"},
  {"sslcrldir", "PGSSLCRLDIR", {}, Support::parse, false, "Trusted OpenSSL hashed directory; enables chain revocation"},
  {"sslsni", "PGSSLSNI", "1", Support::parse, false, "0/1; controls SNI only, never peer verification"},
  {"sslnegotiation",
    "PGSSLNEGOTIATION",
    "postgres",
    Support::parse,
    false,
    "postgres or direct; direct requires verified TLS and PostgreSQL ALPN"},
  {"sslcertmode", "PGSSLCERTMODE", "allow", Support::parse, false, "disable/allow/require; require needs TLS identity"},
  {"ssl", "", {}, Support::unsupported, false, "JDBC URI alias is not implemented"},
  {"requirepeer",
    "PGREQUIREPEER",
    {},
    Support::load,
    false,
    "OS account name resolves to Linux UID; nonempty Windows values are unsupported"},
  {"requiressl",
    "PGREQUIRESSL",
    {},
    Support::unsupported,
    false,
    "Legacy alias; load ignores it when sslmode is supplied"},
  {"sslkeylogfile", "", {}, Support::parse, true, "Explicit sensitive TLS traffic-key log; no ambient opt-in"}};

const OptionDescriptor *descriptor(std::string_view key) noexcept
{
  auto found = std::ranges::find(schema, key, &OptionDescriptor::keyword);
  return found == std::end(schema) ? nullptr : found;
}

Result<void> convert(Options &options, const Fields &fields)
{
  for (const auto &[key, value] : fields.values) {
    auto option = descriptor(key);
    if (!option || option->support == Support::unrecognized)
      return std::unexpected(invalid());
    if (option->support == Support::parse)
      continue;
    return std::unexpected(unsupported());
  }

  if (auto result = hosts(options, fields); !result)
    return result;

  if (auto value = fields.get("user"))
    options.user = *value;
  if (auto value = fields.get("dbname"))
    options.database = *value;
  if (auto value = fields.get("password"))
    options.password = *value;

  auto issuer = fields.get("oauth_issuer");
  auto client_id = fields.get("oauth_client_id");
  auto scope = fields.get("oauth_scope");
  auto secret = fields.get("oauth_client_secret");
  if (issuer || client_id || scope || secret) {
    options.oauth.emplace();
    if (issuer)
      options.oauth->issuer = *issuer;
    if (client_id)
      options.oauth->client_id = *client_id;
    if (scope)
      options.oauth->scope = *scope;
    if (secret && !secret->empty()) {
      auto parsed = OAuthClientSecret::parse(*secret);
      if (!parsed)
        return std::unexpected(parsed.error());
      options.oauth->client_secret = std::move(*parsed);
    }
  }

  if (auto value = fields.get("scram_client_key")) {
    auto key = ScramKey::parse(*value);
    if (!key)
      return std::unexpected(key.error());
    options.scram_client_key = std::move(*key);
  }
  if (auto value = fields.get("scram_server_key")) {
    auto key = ScramKey::parse(*value);
    if (!key)
      return std::unexpected(key.error());
    options.scram_server_key = std::move(*key);
  }
  if (auto value = fields.get("fallback_application_name"); value && !value->empty())
    options.application_name = *value;
  if (auto value = fields.get("application_name"); value && !value->empty())
    options.application_name = *value;
  if (auto value = fields.get("options"))
    options.server_options = *value;
  if (auto value = fields.get("client_encoding"); value && !value->empty()) {
    if (*value == "auto")
      return std::unexpected(unsupported());
    options.client_encoding = *value;
  }
  if (auto value = fields.get("connect_timeout"); value && !value->empty()) {
    auto seconds = number<u32>(*value);
    if (!seconds || *seconds == 0 || *seconds > 86400)
      return std::unexpected(invalid());
    options.connect_timeout = std::chrono::seconds{*seconds};
  }
  if (auto value = fields.get("channel_binding"); value && !value->empty()) {
    if (*value == "disable")
      options.channel_binding = ChannelBinding::disable;
    else if (*value == "prefer")
      options.channel_binding = ChannelBinding::prefer;
    else if (*value == "require")
      options.channel_binding = ChannelBinding::require;
    else
      return std::unexpected(invalid());
  }
  if (auto value = fields.get("require_auth"); value && !value->empty()) {
    auto policy = authentication_policy(*value);
    if (!policy)
      return std::unexpected(policy.error());
    options.authentication = std::move(*policy);
  }
  if (auto value = fields.get("min_protocol_version"); value && !value->empty()) {
    auto version = protocol_version(*value);
    if (!version)
      return std::unexpected(version.error());
    options.min_protocol = *version;
  }
  if (auto value = fields.get("max_protocol_version"); value && !value->empty()) {
    auto version = protocol_version(*value);
    if (!version)
      return std::unexpected(version.error());
    options.max_protocol = *version;
  }
  if (options.min_protocol > options.max_protocol)
    return std::unexpected(invalid());

  if (auto value = fields.get("target_session_attrs"); value && !value->empty()) {
    const std::array labels{"any", "read-write", "read-only", "primary", "standby", "prefer-standby"};
    auto found = std::ranges::find(labels, *value);
    if (found == labels.end())
      return std::unexpected(invalid());
    options.target_session = static_cast<TargetSession>(found - labels.begin());
  }
  if (auto value = fields.get("replication"); value && !value->empty()) {
    auto label = *value;
    for (auto &byte : label) {
      if (byte >= 'A' && byte <= 'Z')
        byte = static_cast<char>(byte - 'A' + 'a');
    }
    const std::array enabled{"true", "on", "yes", "1"};
    const std::array disabled{"false", "off", "no", "0"};
    if (label == "database")
      options.replication = Replication::database;
    else if (std::ranges::find(enabled, label) != enabled.end())
      options.replication = Replication::physical;
    else if (std::ranges::find(disabled, label) == disabled.end())
      return std::unexpected(invalid());
  }

  const std::array
    socket_fields{"keepalives", "keepalives_idle", "keepalives_interval", "keepalives_count", "tcp_user_timeout"};
  for (auto key : socket_fields) {
    auto value = fields.get(key);
    if (!value || value->empty())
      continue;
    auto parsed = number<int>(*value);
    if (!parsed || *parsed < 0)
      return std::unexpected(invalid());

    auto name = std::string_view{key};
    if (name == "keepalives")
      options.keep_alive.enabled = *parsed != 0;
    else if (name == "keepalives_idle")
      options.keep_alive.idle = std::chrono::seconds{*parsed};
    else if (name == "keepalives_interval")
      options.keep_alive.interval = std::chrono::seconds{*parsed};
    else if (name == "keepalives_count")
      options.keep_alive.probes = static_cast<u32>(*parsed);
    else {
#if defined(_WIN32)
      if (*parsed != 0)
        return std::unexpected(unsupported());
#endif
      options.tcp_user_timeout = std::chrono::milliseconds{*parsed};
    }
  }
  if (auto value = fields.get("load_balance_hosts"); value && !value->empty()) {
    if (*value == "random")
      options.host_balance = HostBalance::random;
    else if (*value != "disable")
      return std::unexpected(invalid());
  }
  if (auto value = fields.get("gssencmode"); value && !value->empty()) {
    if (*value == "disable")
      options.gss_encryption = GssEncryption::disable;
    else if (*value == "prefer")
      options.gss_encryption = GssEncryption::prefer;
    else if (*value == "require")
      options.gss_encryption = GssEncryption::require;
    else
      return std::unexpected(invalid());
  }
  if (auto value = fields.get("krbsrvname"); value && !value->empty()) {
    if (!detail::valid_gss_target("localhost", *value))
      return std::unexpected(invalid());
    options.gss_service = *value;
  }
  if (auto value = fields.get("gssdelegation"); value && !value->empty()) {
    if (*value != "0" && *value != "1")
      return std::unexpected(invalid());
    options.gss_delegation = *value == "1";
  }
  if (auto value = fields.get("gsslib"); value && !value->empty()) {
    if (*value != "gssapi" && *value != "sspi")
      return std::unexpected(invalid());
#if defined(_WIN32)
    if (*value != "sspi")
#else
    if (*value != "gssapi")
#endif
      return std::unexpected(unsupported());
  }

  if (auto value = fields.get("sslmode"); value && !value->empty()) {
    if (*value == "disable") {
      options.plaintext = true;
      options.tls_mode = TlsMode::disable;
    } else if (*value == "allow")
      options.tls_mode = TlsMode::allow;
    else if (*value == "prefer")
      options.tls_mode = TlsMode::prefer;
    else if (*value == "require")
      options.tls_mode = TlsMode::require;
    else if (*value == "verify-ca")
      options.tls_mode = TlsMode::verify_ca;
    else if (*value != "verify-full")
      return std::unexpected(invalid());
  }

  if (auto value = fields.get("sslnegotiation"); value && !value->empty()) {
    if (*value == "direct")
      options.tls_negotiation = TlsNegotiation::direct;
    else if (*value != "postgres")
      return std::unexpected(invalid());
  }
  if (options.tls_mode < TlsMode::require && options.tls_negotiation == TlsNegotiation::direct)
    return std::unexpected(invalid());

  if (auto value = fields.get("sslsni"); value && !value->empty()) {
    if (*value != "0" && *value != "1")
      return std::unexpected(invalid());
    options.server_name_indication = *value == "1";
  }

  if (auto value = fields.get("sslcertmode"); value && !value->empty()) {
    if (*value == "disable")
      options.client_certificate = TlsCertificateMode::disable;
    else if (*value == "require")
      options.client_certificate = TlsCertificateMode::require;
    else if (*value != "allow")
      return std::unexpected(invalid());
  }

  TlsClientOptions tls;
  TlsPasswordCleanup cleanup{tls};
  bool configured = false;
  const std::array
    tls_fields{"sslrootcert", "sslcert", "sslkey", "sslpassword", "sslcrl", "sslcrldir", "sslkeylogfile"};
  for (auto key : tls_fields) {
    auto value = fields.get(key);
    if (!value || value->empty())
      continue;
    configured = true;
    if (key == std::string_view{"sslrootcert"}) {
      if (*value != "system")
        tls.ca_file = *value;
    } else if (key == std::string_view{"sslcert"})
      tls.certificate_file = *value;
    else if (key == std::string_view{"sslkey"})
      tls.private_key_file = *value;
    else if (key == std::string_view{"sslpassword"})
      tls.private_key_password = *value;
    else if (key == std::string_view{"sslkeylogfile"})
      tls.key_log_file = *value;
    else {
      if (key == std::string_view{"sslcrl"})
        tls.crl_file = *value;
      else
        tls.crl_directory = *value;
      tls.revocation = TlsRevocation::chain;
    }
  }
  if (auto value = fields.get("ssl_min_protocol_version"); value && !value->empty()) {
    auto version = tls_version(*value);
    if (!version)
      return std::unexpected(version.error());
    tls.min_version = *version;
    configured = true;
  }
  if (auto value = fields.get("ssl_max_protocol_version"); value && !value->empty()) {
    auto version = tls_version(*value);
    if (!version)
      return std::unexpected(version.error());
    tls.max_version = *version;
    configured = true;
  }
  if (tls.min_version > tls.max_version || (options.plaintext && configured) ||
    (options.plaintext && options.channel_binding == ChannelBinding::require))
    return std::unexpected(invalid());
  if (options.tls_mode != TlsMode::verify_full && options.tls_mode != TlsMode::disable) {
    tls.verification = options.tls_mode == TlsMode::verify_ca || !tls.ca_file.empty() ? TlsVerification::certificate
                                                                                      : TlsVerification::none;
    configured = true;
  }
  if (auto root = fields.get("sslrootcert"); root && *root == "system" && options.tls_mode != TlsMode::verify_full)
    return std::unexpected(invalid());
  if (configured)
    options.tls_options = std::move(tls);
  return {};
}

} // namespace

std::span<const OptionDescriptor> option_schema() noexcept
{
  return schema;
}

bool detail::local_host(std::string_view host) noexcept
{
  bool drive = host.size() >= 2 && host[1] == ':' &&
    ((host[0] >= 'A' && host[0] <= 'Z') || (host[0] >= 'a' && host[0] <= 'z'));
  if (drive && IpAddress::parse(host))
    drive = false;
  return host.starts_with('/') || host.starts_with('@') || host.starts_with('\\') || drive;
}

Result<std::string> detail::local_socket_path(std::string_view host, u16 port)
{
  if (!local_host(host) || host.empty() || !port || host.find('\0') != std::string_view::npos || host == "@")
    return std::unexpected(invalid());
  bool drive = host.size() >= 2 && host[1] == ':';
  if (drive && (host.size() < 3 || (host[2] != '/' && host[2] != '\\')))
    return std::unexpected(invalid());
#if defined(_WIN32)
  if (host.starts_with('@'))
    return std::unexpected(unsupported());
#else
  if (drive || host.starts_with('\\'))
    return std::unexpected(unsupported());
#endif
  auto suffix = "/.s.PGSQL." + std::to_string(port);
  if (host.size() > 107 || suffix.size() > 107 - host.size())
    return std::unexpected(std::make_error_code(std::errc::filename_too_long));
  return std::string(host) + suffix;
}

Result<void> detail::valid_authentication_policy(const AuthenticationPolicy &policy) noexcept
{
  if (policy.methods.size() > 7 || (policy.exclude && policy.methods.empty()))
    return std::unexpected(invalid());

  bool unavailable = false;
  for (auto method : policy.methods) {
    if (method < Authentication::none || method > Authentication::oauth)
      return std::unexpected(invalid());
    if ((method == Authentication::gss || method == Authentication::sspi) && !Gss::available())
      unavailable = true;
  }
  if (unavailable && !policy.exclude)
    return std::unexpected(unsupported());
  return {};
}

detail::OptionFields::~OptionFields()
{
  for (auto &[key, value] : values)
    OPENSSL_cleanse(value.data(), value.size());
}

Result<void> detail::OptionFields::put(std::string_view key, std::string value)
{
  TextCleanup cleanup{value};
  if (key.empty() || key.size() > maximum_field || value.size() > maximum_field ||
    value.find('\0') != std::string::npos || key.find('\0') != std::string_view::npos ||
    (values.size() >= 128 && !values.contains(key)))
    return std::unexpected(invalid());

  auto &slot = values[std::string{key}];
  OPENSSL_cleanse(slot.data(), slot.size());
  slot = std::move(value);
  return {};
}

const std::string *detail::OptionFields::get(std::string_view key) const
{
  auto found = values.find(key);
  return found == values.end() ? nullptr : &found->second;
}

void detail::clear_tls_credentials(TlsClientOptions &options) noexcept
{
  OPENSSL_cleanse(options.private_key_password.data(), options.private_key_password.size());
  if (options.private_key_format == TlsPrivateKeyFormat::store)
    OPENSSL_cleanse(options.private_key_file.data(), options.private_key_file.size());
  options.private_key_password_provider.reset();
}

void detail::clear_passwords(Options &options) noexcept
{
  if (options.oauth)
    options.oauth->client_secret.reset();
  OPENSSL_cleanse(options.password.data(), options.password.size());
  for (auto &host : options.hosts) {
    if (host.password)
      OPENSSL_cleanse(host.password->data(), host.password->size());
  }
  if (options.tls_options)
    clear_tls_credentials(*options.tls_options);
}

detail::OptionsCleanup::OptionsCleanup(Options &options) noexcept
    : options(options), password_data(options.password.data()), password_size(options.password.size()),
      private_key_data(options.tls_options ? options.tls_options->private_key_password.data() : nullptr),
      private_key_size(options.tls_options ? options.tls_options->private_key_password.size() : 0),
      private_key_locator_data(options.tls_options ? options.tls_options->private_key_file.data() : nullptr),
      private_key_locator_size(
        options.tls_options && options.tls_options->private_key_format == TlsPrivateKeyFormat::store
          ? options.tls_options->private_key_file.size()
          : 0)
{
}

detail::OptionsCleanup::~OptionsCleanup()
{
  if (active) {
    clear_moved_text(options.password, password_data, password_size);
    if (options.tls_options) {
      clear_moved_text(options.tls_options->private_key_password, private_key_data, private_key_size);
      clear_moved_text(options.tls_options->private_key_file, private_key_locator_data, private_key_locator_size);
    }
    clear_passwords(options);
  }
}

detail::OwnedOptions::OwnedOptions(Options &&options) noexcept
    : OwnedOptions(
        std::move(options),
        options.password.data(),
        options.password.size(),
        options.tls_options ? options.tls_options->private_key_password.data() : nullptr,
        options.tls_options ? options.tls_options->private_key_password.size() : 0,
        options.tls_options ? options.tls_options->private_key_file.data() : nullptr,
        options.tls_options && options.tls_options->private_key_format == TlsPrivateKeyFormat::store
          ? options.tls_options->private_key_file.size()
          : 0)
{
}

detail::OwnedOptions::OwnedOptions(
  Options &&options,
  char *password_data,
  std::size_t password_size,
  char *private_key_data,
  std::size_t private_key_size,
  char *private_key_locator_data,
  std::size_t private_key_locator_size) noexcept
    : value(std::move(options))
{
  clear_moved_text(options.password, password_data, password_size);
  if (options.tls_options) {
    clear_moved_text(options.tls_options->private_key_password, private_key_data, private_key_size);
    clear_moved_text(options.tls_options->private_key_file, private_key_locator_data, private_key_locator_size);
  }
  clear_passwords(options);
}

detail::OwnedOptions::OwnedOptions(OwnedOptions &&other) noexcept : OwnedOptions(std::move(other.value))
{
}

detail::OwnedOptions::~OwnedOptions()
{
  clear_passwords(value);
}

Options detail::OwnedOptions::take() && noexcept
{
  OptionsCleanup cleanup{value};
  return Options{std::move(value)};
}

bool detail::known_option(std::string_view key) noexcept
{
  auto option = descriptor(key);
  return option && option->support != Support::unrecognized;
}

Result<detail::SecretText> detail::decode_option_bytes(std::string_view text)
{
  return decode_bytes(text);
}

Result<void> detail::parse_service_options(OptionFields &fields, std::string_view text)
{
  if (text.size() > maximum_input || text.find('\0') != std::string_view::npos)
    return std::unexpected(invalid());
  if (auto parsed = keywords(fields, text, true); !parsed)
    return parsed;
  for (const auto &[key, value] : fields.values) {
    if (!known_option(key))
      return std::unexpected(invalid());
    if (key == "service")
      return std::unexpected(unsupported());
  }
  return {};
}

Result<void> detail::parse_option_fields(OptionFields &fields, std::string_view connection_string)
{
  if (connection_string.size() > maximum_input || connection_string.find('\0') != std::string_view::npos)
    return std::unexpected(invalid());

  if (connection_string.starts_with("postgresql://"))
    return uri(fields, connection_string.substr(13));
  else if (connection_string.starts_with("postgres://"))
    return uri(fields, connection_string.substr(11));
  else
    return keywords(fields, connection_string);
}

Result<Options> detail::options_from_fields(const OptionFields &fields)
{
  Options options;
  OptionsCleanup cleanup{options};
  if (auto result = convert(options, fields); !result)
    return std::unexpected(result.error());
  cleanup.dismiss();
  return options;
}

Result<Options> Options::parse(std::string_view connection_string)
{
  Fields fields;
  if (auto parsed = detail::parse_option_fields(fields, connection_string); !parsed)
    return std::unexpected(parsed.error());
  return detail::options_from_fields(fields);
}

} // namespace weave::pg
