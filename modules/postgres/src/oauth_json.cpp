#include "oauth.hpp"
#include "oauth_http.hpp"
#include "oauth_device.hpp"
#include "oauth_json_types.hpp"
#include <cstddef>

namespace weave::pg::detail {

namespace {

constexpr std::size_t maximum_json = 65536;

bool visible_ascii(std::string_view value) noexcept
{
  return std::ranges::all_of(value, [](unsigned char byte) {
    return byte >= 0x20 && byte <= 0x7e;
  });
}

bool valid_scope(std::string_view scope) noexcept
{
  if (scope.empty())
    return true;
  bool space = true;
  for (unsigned char byte : scope) {
    if (byte == ' ') {
      if (space)
        return false;
      space = true;
    } else {
      if (byte < 0x21 || byte > 0x7e || byte == '"' || byte == '\\')
        return false;
      space = false;
    }
  }
  return !space;
}

Result<std::size_t> issuer_path(std::string_view issuer)
{
  if (issuer.find_first_of("?#") != std::string_view::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  auto endpoint = oauth_https_url(issuer);
  if (!endpoint)
    return std::unexpected(endpoint.error());
  return std::string_view{"https://"}.size() + endpoint->authority.size();
}

bool is_text(const OAuthJsonValue *value) noexcept
{
  return value && std::holds_alternative<OAuthJsonText>(value->data);
}

std::string_view text(const OAuthJsonValue *value) noexcept
{
  return std::get<OAuthJsonText>(value->data);
}

const OAuthJsonValue *member(const OAuthJsonValue *value, std::string_view key) noexcept
{
  auto *object = std::get_if<OAuthJsonMap>(&value->data);
  if (!object)
    return nullptr;
  auto found = object->find(key);
  return found == object->end() ? nullptr : &found->second;
}

const u64 *integer(const OAuthJsonValue *value) noexcept
{
  return value && value->unsigned_integer_token ? std::get_if<u64>(&value->data) : nullptr;
}

Result<void> validate_tree(const OAuthJsonValue &value)
{
  if (auto *object = std::get_if<OAuthJsonMap>(&value.data)) {
    if (object->members > 64)
      return std::unexpected(make_error_code(Error::resource_limit));
    if (object->duplicate)
      return std::unexpected(make_error_code(Error::protocol));
    for (const auto &[key, child] : *object) {
      if (std::string_view{key}.find('\0') != std::string_view::npos)
        return std::unexpected(make_error_code(Error::protocol));
      auto result = validate_tree(child);
      if (!result)
        return result;
    }
  } else if (auto *array = std::get_if<OAuthJsonValue::Array>(&value.data)) {
    if (array->size() > 256)
      return std::unexpected(make_error_code(Error::resource_limit));
    for (const auto &child : *array) {
      auto result = validate_tree(child);
      if (!result)
        return result;
    }
  }
  return {};
}

bool trusted_discovery(std::string_view url, std::string_view issuer, std::size_t offset)
{
  auto origin = issuer.substr(0, offset);
  auto path = issuer.substr(offset);
  const std::array suffixes{"/.well-known/openid-configuration", "/.well-known/oauth-authorization-server"};
  for (auto suffix : suffixes) {
    if (url == std::string{issuer} + suffix || url == std::string{origin} + suffix + std::string{path})
      return true;
  }
  return false;
}

struct JsonObject {
  OAuthJsonValue value;

  const OAuthJsonValue *root() const noexcept
  {
    return &value;
  }

  JsonObject() = default;
  JsonObject(JsonObject &&) noexcept = default;
  JsonObject(const JsonObject &) = delete;
};

Result<JsonObject> json_object(std::span<const std::byte> json)
{
  if (json.empty())
    return std::unexpected(make_error_code(Error::protocol));
  if (json.size() > maximum_json)
    return std::unexpected(make_error_code(Error::resource_limit));

  JsonObject object;
  OAuthJsonContext context;
  const std::string_view input{reinterpret_cast<const char *>(json.data()), json.size()};
  auto parsed = glz::read<weave::json::detail::ReadPolicy{}>(object.value, input, context);
  if (parsed.ec == glz::error_code::exceeded_max_recursive_depth)
    return std::unexpected(make_error_code(Error::resource_limit));
  if (parsed || !std::holds_alternative<OAuthJsonMap>(object.value.data))
    return std::unexpected(make_error_code(Error::protocol));
  if (auto valid = validate_tree(object.value); !valid)
    return std::unexpected(valid.error());
  return object;
}

Result<std::string_view> required_text(const OAuthJsonValue *root, const char *key, std::size_t limit = maximum_json)
{
  auto *value = member(root, key);
  if (!is_text(value) || text(value).empty() || text(value).find('\0') != std::string_view::npos)
    return std::unexpected(make_error_code(Error::protocol));
  if (text(value).size() > limit)
    return std::unexpected(make_error_code(Error::resource_limit));
  return text(value);
}

Result<bool> string_array_contains(const OAuthJsonValue *value, std::string_view wanted)
{
  auto *array = value ? std::get_if<OAuthJsonValue::Array>(&value->data) : nullptr;
  if (!array || array->empty())
    return std::unexpected(make_error_code(Error::protocol));
  bool found = false;
  for (const auto &entry : *array) {
    if (!is_text(&entry) || text(&entry).empty() || text(&entry).find('\0') != std::string_view::npos)
      return std::unexpected(make_error_code(Error::protocol));
    found |= text(&entry) == wanted;
  }
  return found;
}

Result<std::chrono::seconds> seconds(const OAuthJsonValue *value, bool allow_zero = false)
{
  auto number = integer(value);
  if (!number || (!allow_zero && !*number))
    return std::unexpected(make_error_code(Error::protocol));
  if (*number > 86400)
    return std::unexpected(make_error_code(Error::resource_limit));
  return std::chrono::seconds{*number};
}

bool bearer(std::string_view value) noexcept
{
  const auto lower = [](unsigned char byte) {
    return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
  };
  return std::ranges::equal(value, std::string_view{"bearer"}, {}, lower, lower);
}

} // namespace

Result<OAuthIdentity> oauth_identity(std::string_view configured)
{
  auto offset = issuer_path(configured);
  if (!offset)
    return std::unexpected(offset.error());

  constexpr std::string_view prefix = "/.well-known/";
  auto marker = configured.find(prefix, *offset);
  if (marker == std::string_view::npos)
    return OAuthIdentity{.issuer = std::string{configured}};
  if (configured.find(prefix, marker + prefix.size()) != std::string_view::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto suffix = configured.substr(marker + prefix.size());
  const std::array names{std::string_view{"openid-configuration"}, std::string_view{"oauth-authorization-server"}};
  for (auto name : names) {
    if (!suffix.starts_with(name))
      continue;
    auto tail = suffix.substr(name.size());
    if (!tail.empty() && (tail.front() != '/' || marker != *offset))
      continue;

    auto issuer = std::string{configured.substr(0, marker)} + std::string{tail};
    if (!issuer_path(issuer))
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    return OAuthIdentity{.issuer = std::move(issuer), .openid_configuration = std::string{configured}};
  }
  return std::unexpected(std::make_error_code(std::errc::invalid_argument));
}

Result<void> valid_oauth_options(const OAuthOptions &options)
{
  if (!oauth_identity(options.issuer) || options.client_id.empty() || options.client_id.size() > maximum_json ||
    !visible_ascii(options.client_id) ||
    (options.scope && (options.scope->size() > maximum_json || !valid_scope(*options.scope))) ||
    (options.client_secret && options.client_secret->value().empty()) ||
    options.acquisition_timeout <= std::chrono::milliseconds{0} || options.acquisition_timeout > std::chrono::hours{24})
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return {};
}

Result<OAuthDiscovery> oauth_discovery(std::span<const std::byte> json, const OAuthOptions &options)
{
  auto identity = oauth_identity(options.issuer);
  if (!identity)
    return std::unexpected(identity.error());
  auto offset = issuer_path(identity->issuer);
  if (!offset)
    return std::unexpected(offset.error());
  auto object = json_object(json);
  if (!object)
    return std::unexpected(object.error());
  auto *root = object->root();

  auto *status = member(root, "status");
  auto *scope = member(root, "scope");
  auto *url = member(root, "openid-configuration");
  if (!is_text(status) || (scope && !is_text(scope)) || (url && !is_text(url)))
    return std::unexpected(make_error_code(Error::protocol));
  if (text(status) != "invalid_token")
    return std::unexpected(make_error_code(Error::authentication));
  if (scope && !valid_scope(text(scope)))
    return std::unexpected(make_error_code(Error::protocol));
  if (url) {
    bool trusted = identity->openid_configuration ? text(url) == *identity->openid_configuration
                                                  : trusted_discovery(text(url), identity->issuer, *offset);
    if (!trusted)
      return std::unexpected(make_error_code(Error::authentication));
  }

  OAuthDiscovery result;
  if (options.scope)
    result.scope = *options.scope;
  else if (scope)
    result.scope = text(scope);
  if (url)
    result.openid_configuration = text(url);
  else if (identity->openid_configuration)
    result.openid_configuration = *identity->openid_configuration;
  else
    result.openid_configuration = identity->issuer + "/.well-known/openid-configuration";
  return result;
}

Result<void> valid_oauth_request(const OAuthRequest &request)
{
  OAuthOptions options{.issuer = request.issuer, .client_id = request.client_id, .scope = request.scope};
  if (auto valid = valid_oauth_options(options); !valid)
    return valid;
  auto offset = issuer_path(request.issuer);
  if (!offset)
    return std::unexpected(offset.error());
  if (!trusted_discovery(request.openid_configuration, request.issuer, *offset))
    return std::unexpected(make_error_code(Error::authentication));
  return {};
}

Result<OAuthMetadata> oauth_metadata(std::span<const std::byte> json, std::string_view issuer)
{
  auto object = json_object(json);
  if (!object)
    return std::unexpected(object.error());
  auto identity = required_text(object->root(), "issuer", 8192);
  auto device = required_text(object->root(), "device_authorization_endpoint", 8192);
  auto token = required_text(object->root(), "token_endpoint", 8192);
  if (!identity)
    return std::unexpected(identity.error());
  if (!device)
    return std::unexpected(device.error());
  if (!token)
    return std::unexpected(token.error());
  if (*identity != issuer)
    return std::unexpected(make_error_code(Error::authentication));
  if (!oauth_https_url(*device) || !oauth_https_url(*token))
    return std::unexpected(make_error_code(Error::protocol));

  auto device_grant = string_array_contains(member(object->root(), "grant_types_supported"), oauth_device_grant);
  if (!device_grant)
    return std::unexpected(device_grant.error());
  if (!*device_grant)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  OAuthMetadata result{.device_endpoint = std::string{*device}, .token_endpoint = std::string{*token}, .basic = true};
  if (auto *methods = member(object->root(), "token_endpoint_auth_methods_supported")) {
    auto basic = string_array_contains(methods, "client_secret_basic");
    auto post = string_array_contains(methods, "client_secret_post");
    if (!basic)
      return std::unexpected(basic.error());
    if (!post)
      return std::unexpected(post.error());
    result.basic = *basic;
    result.post = *post;
  }
  return result;
}

Result<OAuthGrant> oauth_grant(std::span<const std::byte> json)
{
  auto object = json_object(json);
  if (!object)
    return std::unexpected(object.error());
  auto *root = object->root();
  auto device = required_text(root, "device_code");
  auto user = required_text(root, "user_code", 4096);
  auto verification = required_text(root, "verification_uri", 8192);
  auto lifetime = seconds(member(root, "expires_in"));
  if (!device)
    return std::unexpected(device.error());
  if (!user)
    return std::unexpected(user.error());
  if (!verification)
    return std::unexpected(verification.error());
  if (!lifetime)
    return std::unexpected(lifetime.error());
  if (!visible_ascii(*user) || !oauth_https_url(*verification, true) || member(root, "error"))
    return std::unexpected(make_error_code(Error::protocol));

  OAuthGrant result;
  result.device_code.assign(device->begin(), device->end());
  result.user_code.assign(user->begin(), user->end());
  result.verification_uri.assign(verification->begin(), verification->end());
  result.lifetime = *lifetime;
  if (auto *interval = member(root, "interval")) {
    auto duration = seconds(interval, true);
    if (!duration)
      return std::unexpected(duration.error());
    result.interval = std::max(*duration, std::chrono::seconds{1});
  }
  if (member(root, "verification_uri_complete")) {
    auto complete = required_text(root, "verification_uri_complete", 8192);
    if (!complete || !oauth_https_url(*complete, true))
      return std::unexpected(make_error_code(Error::protocol));
    result.verification_uri_complete.assign(complete->begin(), complete->end());
  }
  return result;
}

Result<OAuthPollResult> oauth_poll(std::span<const std::byte> json, u16 status)
{
  auto object = json_object(json);
  if (!object)
    return std::unexpected(object.error());
  auto *root = object->root();
  if (status != 200) {
    if (member(root, "access_token") || member(root, "token_type"))
      return std::unexpected(make_error_code(Error::protocol));
    auto error = required_text(root, "error", 256);
    if (!error || status != 400)
      return std::unexpected(make_error_code(Error::authentication));
    if (*error == "authorization_pending")
      return OAuthPollResult{OAuthPoll::pending};
    if (*error == "slow_down")
      return OAuthPollResult{OAuthPoll::slow_down};
    if (*error == "expired_token")
      return std::unexpected(std::make_error_code(std::errc::timed_out));
    if (*error == "access_denied")
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
    return std::unexpected(make_error_code(Error::authentication));
  }
  if (member(root, "error"))
    return std::unexpected(make_error_code(Error::protocol));
  auto access = required_text(root, "access_token");
  auto type = required_text(root, "token_type", 256);
  if (!access || !type || !bearer(*type))
    return std::unexpected(make_error_code(Error::protocol));
  if (auto *lifetime = member(root, "expires_in")) {
    auto duration = integer(lifetime);
    if (!duration)
      return std::unexpected(make_error_code(Error::protocol));
    if (!*duration)
      return std::unexpected(std::make_error_code(std::errc::timed_out));
  }
  if (auto *scope = member(root, "scope")) {
    if (!is_text(scope) || !valid_scope(text(scope)))
      return std::unexpected(make_error_code(Error::protocol));
  }
  auto token = OAuthToken::parse(*access);
  if (!token)
    return std::unexpected(make_error_code(Error::protocol));
  return OAuthPollResult{.token = std::move(*token)};
}

} // namespace weave::pg::detail
