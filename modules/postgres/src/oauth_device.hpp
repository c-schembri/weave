#pragma once

#include "oauth.hpp"
#include "oauth_http.hpp"

namespace weave::pg::detail {

inline constexpr std::string_view oauth_device_grant = "urn:ietf:params:oauth:grant-type:device_code";

struct OAuthMetadata {
  std::string device_endpoint;
  std::string token_endpoint;
  bool basic = false;
  bool post = false;
};

struct OAuthGrant {
  OAuthGrant() = default;
  OAuthGrant(OAuthGrant &&) noexcept = default;
  OAuthGrant(const OAuthGrant &) = delete;

  SecretText device_code;
  SecretText user_code;
  SecretText verification_uri;
  SecretText verification_uri_complete;
  std::chrono::seconds lifetime{0};
  std::chrono::seconds interval{5};
};

enum class OAuthPoll {
  accepted,
  pending,
  slow_down
};

struct OAuthPollResult {
  OAuthPoll state = OAuthPoll::accepted;
  std::optional<OAuthToken> token;
};

struct OAuthDeviceAccess {
  static OAuthDevicePrompt prompt(OAuthGrant &grant, std::chrono::steady_clock::time_point expires);
};

Result<void> valid_oauth_request(const OAuthRequest &request);
Result<OAuthMetadata> oauth_metadata(std::span<const std::byte> json, std::string_view issuer);
Result<OAuthGrant> oauth_grant(std::span<const std::byte> json);
Result<OAuthPollResult> oauth_poll(std::span<const std::byte> json, u16 status);

} // namespace weave::pg::detail
