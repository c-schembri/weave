#include <weave/postgres/oauth.hpp>
#include "oauth.hpp"
#include "secret.hpp"
#include <weave/postgres/encoding.hpp>
#include <algorithm>
#include <array>

namespace weave::pg {

namespace {

constexpr std::size_t maximum_oauth_field = 65536;

bool token_character(unsigned char character) noexcept
{
  return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
    (character >= '0' && character <= '9') || character == '-' || character == '.' || character == '_' ||
    character == '~' || character == '+' || character == '/';
}

bool valid_token(std::string_view token) noexcept
{
  if (token.empty() || token.size() > maximum_oauth_field)
    return false;

  bool padding = false;
  for (std::size_t index = 0; index < token.size(); ++index) {
    auto character = static_cast<unsigned char>(token[index]);
    if (character == '=') {
      if (!index)
        return false;
      padding = true;
    } else if (padding || !token_character(character)) {
      return false;
    }
  }

  return true;
}

bool valid_request(const OAuthRequest &request) noexcept
{
  const std::array fields{
    std::string_view{request.issuer},
    std::string_view{request.client_id},
    std::string_view{request.scope},
    std::string_view{request.openid_configuration},
    std::string_view{request.host},
    std::string_view{request.user},
    std::string_view{request.database}};
  bool valid = std::ranges::all_of(fields, [](std::string_view field) {
    return field.size() <= maximum_oauth_field && field.find('\0') == std::string_view::npos;
  });
  return valid && (!request.client_secret || !request.client_secret->value().empty());
}

} // namespace

struct OAuthToken::Impl {
  detail::SecretText token;

  explicit Impl(std::string_view value) : token(value.begin(), value.end())
  {
  }
};

OAuthToken::OAuthToken(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

OAuthToken::OAuthToken(OAuthToken &&) noexcept = default;
OAuthToken &OAuthToken::operator=(OAuthToken &&) noexcept = default;
OAuthToken::~OAuthToken() = default;

struct OAuthClientSecret::Impl {
  detail::SecretText secret;

  explicit Impl(std::string_view value) : secret(value.begin(), value.end())
  {
  }
};

OAuthClientSecret::OAuthClientSecret(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl))
{
}

OAuthClientSecret::OAuthClientSecret(OAuthClientSecret &&) noexcept = default;
OAuthClientSecret &OAuthClientSecret::operator=(OAuthClientSecret &&) noexcept = default;
OAuthClientSecret::~OAuthClientSecret() = default;

Result<OAuthClientSecret> OAuthClientSecret::parse(std::string_view secret)
{
  if (secret.empty() || secret.size() > maximum_oauth_field || !validate_text(secret))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return OAuthClientSecret{std::make_shared<Impl>(secret)};
}

std::string_view OAuthClientSecret::value() const noexcept
{
  return impl_ ? std::string_view{impl_->secret.data(), impl_->secret.size()} : std::string_view{};
}

Result<OAuthToken> OAuthToken::parse(std::string_view token)
{
  if (!valid_token(token))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  return OAuthToken{std::make_unique<Impl>(token)};
}

std::string_view OAuthToken::value() const noexcept
{
  if (!impl_)
    return {};

  return {impl_->token.data(), impl_->token.size()};
}

struct OAuthProvider::Impl {
  Factory factory;
  CacheLookup cached;

  Impl(Factory value, CacheLookup lookup) : factory(std::move(value)), cached(std::move(lookup))
  {
  }
};

OAuthProvider::OAuthProvider(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl))
{
}

Result<OAuthProvider> OAuthProvider::create(Factory factory, CacheLookup cached)
{
  if (!factory)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  return OAuthProvider{std::make_shared<Impl>(std::move(factory), std::move(cached))};
}

Result<std::optional<OAuthToken>> OAuthProvider::cached_token(const OAuthRequest &request) const
{
  auto impl = impl_;
  weave::detail::require(impl != nullptr);
  if (!valid_request(request))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (!impl->cached)
    return std::optional<OAuthToken>{};

  auto token = impl->cached(request);
  if (token && *token && (*token)->value().empty())
    return std::unexpected(make_error_code(Error::authentication));
  return token;
}

Task<OAuthToken> OAuthProvider::acquire(std::shared_ptr<const Impl> impl, OAuthRequest request)
{
  co_await cancellation_point();
  if (!valid_request(request))
    co_await fail(std::errc::invalid_argument);

  // The parameter owner retains even a coroutine-lambda factory until its child drains.
  co_return co_await impl->factory(std::move(request));
}

Task<OAuthToken> OAuthProvider::request(OAuthRequest request) const
{
  weave::detail::require(impl_ != nullptr);
  return acquire(impl_, std::move(request));
}

} // namespace weave::pg
