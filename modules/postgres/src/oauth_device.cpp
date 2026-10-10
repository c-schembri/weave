#include <weave/postgres/oauth.hpp>
#include "oauth_device.hpp"
#include <weave/timer.hpp>
#include <openssl/evp.h>
#include <algorithm>

namespace weave::pg {

struct OAuthDevicePrompt::Impl {
  detail::SecretText verification_uri;
  detail::SecretText user_code;
  detail::SecretText verification_uri_complete;
  std::chrono::steady_clock::time_point expires;
};

OAuthDevicePrompt::OAuthDevicePrompt(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

OAuthDevicePrompt::OAuthDevicePrompt(OAuthDevicePrompt &&) noexcept = default;
OAuthDevicePrompt &OAuthDevicePrompt::operator=(OAuthDevicePrompt &&) noexcept = default;
OAuthDevicePrompt::~OAuthDevicePrompt() = default;

std::string_view OAuthDevicePrompt::verification_uri() const noexcept
{
  return impl_ ? std::string_view{impl_->verification_uri.data(), impl_->verification_uri.size()} : std::string_view{};
}

std::string_view OAuthDevicePrompt::user_code() const noexcept
{
  return impl_ ? std::string_view{impl_->user_code.data(), impl_->user_code.size()} : std::string_view{};
}

std::string_view OAuthDevicePrompt::verification_uri_complete() const noexcept
{
  return impl_ ? std::string_view{impl_->verification_uri_complete.data(), impl_->verification_uri_complete.size()}
               : std::string_view{};
}

std::chrono::steady_clock::time_point OAuthDevicePrompt::expires_at() const noexcept
{
  return impl_ ? impl_->expires : std::chrono::steady_clock::time_point{};
}

OAuthDevicePrompt detail::OAuthDeviceAccess::prompt(OAuthGrant &grant, std::chrono::steady_clock::time_point expires)
{
  auto impl = std::make_unique<OAuthDevicePrompt::Impl>();
  impl->verification_uri = std::move(grant.verification_uri);
  impl->user_code = std::move(grant.user_code);
  impl->verification_uri_complete = std::move(grant.verification_uri_complete);
  impl->expires = expires;
  return OAuthDevicePrompt{std::move(impl)};
}

namespace {

using Clock = std::chrono::steady_clock;

struct DeviceState {
  OAuthProvider::DeviceHandler handler;
  TlsContext tls;
  std::optional<OAuthClientSecret> secret;
  OAuthClientAuth authentication;
  std::chrono::milliseconds timeout;
};

std::string_view view(const detail::SecretText &text) noexcept
{
  return {text.data(), text.size()};
}

// Form encoding is serialization, not HTTP parsing. Secret intermediates never use ordinary strings.
void form_encode(detail::SecretText &destination, std::string_view value)
{
  constexpr std::string_view hex = "0123456789ABCDEF";
  for (unsigned char byte : value) {
    bool literal = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
      byte == '-' || byte == '_' || byte == '.' || byte == '*';
    if (literal) {
      destination.push_back(static_cast<char>(byte));
    } else if (byte == ' ') {
      destination.push_back('+');
    } else {
      destination.push_back('%');
      destination.push_back(hex[byte >> 4]);
      destination.push_back(hex[byte & 15]);
    }
  }
}

void field(detail::SecretText &body, std::string_view name, std::string_view value)
{
  if (!body.empty())
    body.push_back('&');
  body.insert(body.end(), name.begin(), name.end());
  body.push_back('=');
  form_encode(body, value);
}

detail::SecretText basic(std::string_view client, std::string_view secret)
{
  detail::SecretText plaintext;
  form_encode(plaintext, client);
  plaintext.push_back(':');
  form_encode(plaintext, secret);
  auto size = 4 * ((plaintext.size() + 2) / 3);
  detail::SecretText result{'B', 'a', 's', 'i', 'c', ' '};
  result.resize(6 + size + 1);
  auto length = EVP_EncodeBlock(
    reinterpret_cast<unsigned char *>(result.data() + 6),
    reinterpret_cast<const unsigned char *>(plaintext.data()),
    static_cast<int>(plaintext.size()));
  weave::detail::require(length == static_cast<int>(size));
  result.resize(6 + size);
  return result;
}

Result<OAuthClientAuth> authentication(
  const DeviceState &state,
  const detail::OAuthMetadata &metadata,
  bool confidential)
{
  auto method = state.authentication;
  if (method == OAuthClientAuth::automatic) {
    if (!confidential)
      return OAuthClientAuth::none;
    if (metadata.basic)
      return OAuthClientAuth::client_secret_basic;
    if (metadata.post)
      return OAuthClientAuth::client_secret_post;
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  }
  bool supported = method == OAuthClientAuth::none ||
    (method == OAuthClientAuth::client_secret_basic && metadata.basic) ||
    (method == OAuthClientAuth::client_secret_post && metadata.post);
  if (!supported)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  return method;
}

detail::OAuthHttpRequest post(
  OAuthClientAuth method,
  std::string url,
  std::string_view client,
  std::string_view scope,
  std::string_view secret,
  std::string_view code = {})
{
  detail::OAuthHttpRequest request{.url = std::move(url), .post = true};
  if (!code.empty()) {
    field(request.body, "grant_type", detail::oauth_device_grant);
    field(request.body, "device_code", code);
  } else if (!scope.empty()) {
    field(request.body, "scope", scope);
  }
  if (method == OAuthClientAuth::client_secret_basic) {
    request.authorization = basic(client, secret);
  } else {
    field(request.body, "client_id", client);
    if (method == OAuthClientAuth::client_secret_post)
      field(request.body, "client_secret", secret);
  }
  return request;
}

Task<OAuthToken> poll(
  const DeviceState &state,
  detail::OAuthMetadata metadata,
  detail::OAuthGrant grant,
  OAuthRequest request,
  std::optional<OAuthClientSecret> secret,
  OAuthClientAuth method,
  Clock::time_point expires)
{
  auto prompt = detail::OAuthDeviceAccess::prompt(grant, expires);
  co_await state.handler(std::move(prompt));

  auto interval = grant.interval;
  auto credential = secret ? secret->value() : std::string_view{};
  constexpr auto maximum_interval = std::chrono::seconds{86400};
  for (;;) {
    co_await sleep_for(interval);
    auto outgoing = post(method, metadata.token_endpoint, request.client_id, {}, credential, view(grant.device_code));
    auto response = co_await as_result(detail::oauth_https(std::move(outgoing), state.tls, state.timeout));
    co_await cancellation_point();
    if (Clock::now() >= expires)
      co_await fail(std::make_error_code(std::errc::timed_out));
    if (!response) {
      if (response.error() != std::errc::timed_out)
        co_await fail(response.error());
      interval = std::min(interval * 2, maximum_interval);
      continue;
    }
    auto result = detail::oauth_poll(std::as_bytes(std::span{response->body}), response->status);
    if (!result)
      co_await fail(result.error());
    if (result->state == detail::OAuthPoll::accepted) {
      weave::detail::require(result->token.has_value());
      co_return std::move(*result->token);
    }
    if (result->state == detail::OAuthPoll::slow_down)
      interval = std::min(interval + std::chrono::seconds{5}, maximum_interval);
  }
}

Task<OAuthToken> device_flow(const DeviceState &state, OAuthRequest request)
{
  co_await cancellation_point();
  if (auto valid = detail::valid_oauth_request(request); !valid)
    co_await fail(valid.error());

  // Connection credentials take precedence over a provider's optional default.
  auto secret = std::move(request.client_secret);
  if (!secret)
    secret = state.secret;
  bool requires_secret = state.authentication == OAuthClientAuth::client_secret_basic ||
    state.authentication == OAuthClientAuth::client_secret_post;
  if ((requires_secret && !secret) || (state.authentication == OAuthClientAuth::none && secret))
    co_await fail(std::errc::invalid_argument);

  auto discovery = co_await detail::oauth_https({.url = request.openid_configuration}, state.tls, state.timeout);
  if (discovery.status != 200)
    co_await fail(Error::authentication);
  auto metadata = detail::oauth_metadata(std::as_bytes(std::span{discovery.body}), request.issuer);
  detail::SecretText{}.swap(discovery.body);
  if (!metadata)
    co_await fail(metadata.error());
  auto method = authentication(state, *metadata, secret.has_value());
  if (!method)
    co_await fail(method.error());

  auto started = Clock::now();
  auto credential = secret ? secret->value() : std::string_view{};
  auto outgoing = post(*method, metadata->device_endpoint, request.client_id, request.scope, credential);
  auto response = co_await detail::oauth_https(std::move(outgoing), state.tls, state.timeout);
  if (response.status != 200)
    co_await fail(Error::authentication);
  auto grant = detail::oauth_grant(std::as_bytes(std::span{response.body}));
  detail::SecretText{}.swap(response.body);
  if (!grant)
    co_await fail(grant.error());
  // Conservatively charge request latency against the code lifetime, including prompt work and polling.
  auto lifetime = std::chrono::duration_cast<Clock::duration>(grant->lifetime);
  if (started > Clock::time_point::max() - lifetime)
    co_await fail(Error::resource_limit);
  auto expires = started + lifetime;
  if (Clock::now() >= expires)
    co_await fail(std::make_error_code(std::errc::timed_out));
  co_return co_await timeout_at(
    expires,
    poll(state, std::move(*metadata), std::move(*grant), std::move(request), std::move(secret), *method, expires));
}

} // namespace

Result<OAuthProvider> OAuthProvider::device(DeviceHandler handler, OAuthDeviceOptions options, CacheLookup cached)
{
  if (!handler || options.request_timeout <= std::chrono::milliseconds{0} ||
    options.request_timeout > std::chrono::hours{24} ||
    (options.client_secret && options.client_secret->value().empty()))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  switch (options.client_auth) {
  case OAuthClientAuth::automatic:
    break;
  case OAuthClientAuth::none:
    if (options.client_secret)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    break;
  case OAuthClientAuth::client_secret_basic:
  case OAuthClientAuth::client_secret_post:
    break;
  default:
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }
  if (!options.tls) {
    auto tls = TlsContext::client({.alpn = {"http/1.1"}});
    if (!tls)
      return std::unexpected(tls.error());
    options.tls = std::move(*tls);
  }
  DeviceState state{
    std::move(handler),
    std::move(*options.tls),
    std::move(options.client_secret),
    options.client_auth,
    options.request_timeout};
  return create(
    [state = std::move(state)](OAuthRequest request) noexcept {
      return device_flow(state, std::move(request));
    },
    std::move(cached));
}

} // namespace weave::pg
