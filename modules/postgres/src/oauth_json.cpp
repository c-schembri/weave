#include "oauth.hpp"
#include "oauth_http.hpp"
#include "oauth_device.hpp"
#include <cstddef>

// Upstream remains unmodified. Its C API has TU-local linkage in this wrapper.
#define yyjson_api static
#define YYJSON_DISABLE_WRITER 1
#define YYJSON_DISABLE_INCR_READER 1
#define YYJSON_DISABLE_UTILS 1
#define YYJSON_DISABLE_NON_STANDARD 1
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4505)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#endif
#include "vendor/yyjson/yyjson.c"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace weave::pg::detail {

namespace {

constexpr std::size_t maximum_json = 65536;
constexpr std::size_t maximum_nodes = 4096;
constexpr std::size_t maximum_depth = 8;

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

std::string_view text(yyjson_val *value) noexcept
{
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

Result<void> validate_tree(yyjson_val *value, std::size_t depth, std::size_t &nodes)
{
  bool container = yyjson_is_obj(value) || yyjson_is_arr(value);
  if (++nodes > maximum_nodes || (container && depth > maximum_depth))
    return std::unexpected(make_error_code(Error::resource_limit));
  if (yyjson_is_obj(value)) {
    if (yyjson_obj_size(value) > 64)
      return std::unexpected(make_error_code(Error::resource_limit));
    std::array<std::string_view, 64> keys;
    std::size_t count = 0;
    auto iterator = yyjson_obj_iter_with(value);
    while (auto *key = yyjson_obj_iter_next(&iterator)) {
      auto name = text(key);
      auto used = std::span{keys}.first(count);
      if (name.find('\0') != std::string_view::npos || std::ranges::find(used, name) != used.end())
        return std::unexpected(make_error_code(Error::protocol));
      keys[count++] = name;
      auto result = validate_tree(yyjson_obj_iter_get_val(key), depth + 1, nodes);
      if (!result)
        return result;
    }
  } else if (yyjson_is_arr(value)) {
    if (yyjson_arr_size(value) > 256)
      return std::unexpected(make_error_code(Error::resource_limit));
    auto iterator = yyjson_arr_iter_with(value);
    while (auto *item = yyjson_arr_iter_next(&iterator)) {
      auto result = validate_tree(item, depth + 1, nodes);
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
  SecretStorage<std::max_align_t> memory;
  yyjson_val *root = nullptr;

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
  auto size = yyjson_read_max_memory_usage(json.size(), 0);
  object.memory.resize((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
  yyjson_alc allocator;
  if (!yyjson_alc_pool_init(&allocator, object.memory.data(), object.memory.size() * sizeof(std::max_align_t)))
    return std::unexpected(make_error_code(Error::resource_limit));
  // Decoded strings remain in the owned cleansing pool; input is never modified.
  auto *document = yyjson_read_opts(
    const_cast<char *>(reinterpret_cast<const char *>(json.data())),
    json.size(),
    0,
    &allocator,
    nullptr);
  if (!document || !yyjson_is_obj(yyjson_doc_get_root(document)))
    return std::unexpected(make_error_code(Error::protocol));
  object.root = yyjson_doc_get_root(document);
  std::size_t nodes = 0;
  if (auto valid = validate_tree(object.root, 1, nodes); !valid)
    return std::unexpected(valid.error());
  return object;
}

Result<std::string_view> required_text(yyjson_val *root, const char *key, std::size_t limit = maximum_json)
{
  auto *value = yyjson_obj_get(root, key);
  if (!yyjson_is_str(value) || text(value).empty() || text(value).find('\0') != std::string_view::npos)
    return std::unexpected(make_error_code(Error::protocol));
  if (text(value).size() > limit)
    return std::unexpected(make_error_code(Error::resource_limit));
  return text(value);
}

Result<bool> string_array_contains(yyjson_val *value, std::string_view wanted)
{
  if (!yyjson_is_arr(value) || !yyjson_arr_size(value))
    return std::unexpected(make_error_code(Error::protocol));
  bool found = false;
  auto iterator = yyjson_arr_iter_with(value);
  while (auto *entry = yyjson_arr_iter_next(&iterator)) {
    if (!yyjson_is_str(entry) || text(entry).empty() || text(entry).find('\0') != std::string_view::npos)
      return std::unexpected(make_error_code(Error::protocol));
    found |= text(entry) == wanted;
  }
  return found;
}

Result<std::chrono::seconds> seconds(yyjson_val *value, bool allow_zero = false)
{
  if (!yyjson_is_uint(value) || (!allow_zero && !yyjson_get_uint(value)))
    return std::unexpected(make_error_code(Error::protocol));
  auto number = yyjson_get_uint(value);
  if (number > 86400)
    return std::unexpected(make_error_code(Error::resource_limit));
  return std::chrono::seconds{number};
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
  auto *root = object->root;

  auto *status = yyjson_obj_get(root, "status");
  auto *scope = yyjson_obj_get(root, "scope");
  auto *url = yyjson_obj_get(root, "openid-configuration");
  if (!yyjson_is_str(status) || (scope && !yyjson_is_str(scope)) || (url && !yyjson_is_str(url)))
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
  auto identity = required_text(object->root, "issuer", 8192);
  auto device = required_text(object->root, "device_authorization_endpoint", 8192);
  auto token = required_text(object->root, "token_endpoint", 8192);
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

  auto device_grant = string_array_contains(yyjson_obj_get(object->root, "grant_types_supported"), oauth_device_grant);
  if (!device_grant)
    return std::unexpected(device_grant.error());
  if (!*device_grant)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  OAuthMetadata result{.device_endpoint = std::string{*device}, .token_endpoint = std::string{*token}, .basic = true};
  if (auto *methods = yyjson_obj_get(object->root, "token_endpoint_auth_methods_supported")) {
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
  auto *root = object->root;
  auto device = required_text(root, "device_code");
  auto user = required_text(root, "user_code", 4096);
  auto verification = required_text(root, "verification_uri", 8192);
  auto lifetime = seconds(yyjson_obj_get(root, "expires_in"));
  if (!device)
    return std::unexpected(device.error());
  if (!user)
    return std::unexpected(user.error());
  if (!verification)
    return std::unexpected(verification.error());
  if (!lifetime)
    return std::unexpected(lifetime.error());
  if (!visible_ascii(*user) || !oauth_https_url(*verification, true) || yyjson_obj_get(root, "error"))
    return std::unexpected(make_error_code(Error::protocol));

  OAuthGrant result;
  result.device_code.assign(device->begin(), device->end());
  result.user_code.assign(user->begin(), user->end());
  result.verification_uri.assign(verification->begin(), verification->end());
  result.lifetime = *lifetime;
  if (auto *interval = yyjson_obj_get(root, "interval")) {
    auto duration = seconds(interval, true);
    if (!duration)
      return std::unexpected(duration.error());
    result.interval = std::max(*duration, std::chrono::seconds{1});
  }
  if (yyjson_obj_get(root, "verification_uri_complete")) {
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
  auto *root = object->root;
  if (status != 200) {
    if (yyjson_obj_get(root, "access_token") || yyjson_obj_get(root, "token_type"))
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
  if (yyjson_obj_get(root, "error"))
    return std::unexpected(make_error_code(Error::protocol));
  auto access = required_text(root, "access_token");
  auto type = required_text(root, "token_type", 256);
  if (!access || !type || !bearer(*type))
    return std::unexpected(make_error_code(Error::protocol));
  if (auto *lifetime = yyjson_obj_get(root, "expires_in")) {
    if (!yyjson_is_uint(lifetime))
      return std::unexpected(make_error_code(Error::protocol));
    if (!yyjson_get_uint(lifetime))
      return std::unexpected(std::make_error_code(std::errc::timed_out));
  }
  if (auto *scope = yyjson_obj_get(root, "scope")) {
    if (!yyjson_is_str(scope) || !valid_scope(text(scope)))
      return std::unexpected(make_error_code(Error::protocol));
  }
  auto token = OAuthToken::parse(*access);
  if (!token)
    return std::unexpected(make_error_code(Error::protocol));
  return OAuthPollResult{.token = std::move(*token)};
}

} // namespace weave::pg::detail
