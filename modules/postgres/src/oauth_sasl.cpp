#include "oauth.hpp"

namespace weave::pg::detail {

Result<SecretWriter> OAuthExchange::start(std::span<const std::byte> mechanisms, const OAuthToken &token)
{
  if (token.value().empty())
    return std::unexpected(make_error_code(Error::authentication));
  return initial(mechanisms, &token);
}

Result<SecretWriter> OAuthExchange::discover(std::span<const std::byte> mechanisms)
{
  return initial(mechanisms, nullptr);
}

Result<bool> oauth_offered(std::span<const std::byte> mechanisms)
{
  bool advertised = false;
  bool ended = false;
  unsigned count = 0;
  while (!mechanisms.empty()) {
    auto terminator = std::ranges::find(mechanisms, std::byte{});
    if (terminator == mechanisms.end())
      return std::unexpected(make_error_code(Error::protocol));

    auto size = static_cast<std::size_t>(terminator - mechanisms.begin());
    if (!size) {
      mechanisms = mechanisms.subspan(1);
      ended = true;
      break;
    }
    if (++count > 64 || size > 256)
      return std::unexpected(make_error_code(Error::resource_limit));

    std::string_view mechanism{reinterpret_cast<const char *>(mechanisms.data()), size};
    mechanisms = mechanisms.subspan(size + 1);
    if (mechanism == "OAUTHBEARER") {
      if (advertised)
        return std::unexpected(make_error_code(Error::protocol));
      advertised = true;
    }
  }
  if (!ended || !mechanisms.empty())
    return std::unexpected(make_error_code(Error::protocol));
  return advertised;
}

Result<SecretWriter> OAuthExchange::initial(std::span<const std::byte> mechanisms, const OAuthToken *token)
{
  if (state_ != State::initial)
    return std::unexpected(make_error_code(Error::authentication));
  auto advertised = oauth_offered(mechanisms);
  if (!advertised)
    return std::unexpected(advertised.error());
  if (!*advertised)
    return std::unexpected(make_error_code(Error::unsupported_authentication));

  SecretWriter body;
  body.raw("n,,");
  body.integer(1, 1);
  body.raw("auth=");
  if (token) {
    body.raw("Bearer ");
    body.raw(token->value());
  }
  body.integer(1, 1);
  body.integer(1, 1);
  SecretWriter response;
  response.string("OAUTHBEARER");
  response.integer(static_cast<u32>(body.bytes.size()));
  response.raw(body.bytes);
  state_ = State::awaiting;
  return response;
}

Result<SecretWriter> OAuthExchange::reject()
{
  if (state_ != State::awaiting)
    return std::unexpected(make_error_code(Error::protocol));
  state_ = State::rejected;
  SecretWriter response;
  response.integer(1, 1);
  return response;
}

Result<void> OAuthExchange::accept()
{
  if (state_ != State::awaiting)
    return std::unexpected(make_error_code(Error::authentication));
  state_ = State::accepted;
  return {};
}

bool OAuthExchange::complete() const noexcept
{
  return state_ == State::accepted;
}

} // namespace weave::pg::detail
