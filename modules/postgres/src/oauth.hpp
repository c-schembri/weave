#pragma once

#include "auth.hpp"
#include <weave/postgres/oauth.hpp>

namespace weave::pg::detail {

struct OAuthDiscovery {
  std::string scope;
  std::string openid_configuration;
};

struct OAuthIdentity {
  std::string issuer;
  std::optional<std::string> openid_configuration;
};

Result<OAuthIdentity> oauth_identity(std::string_view configured);
Result<void> valid_oauth_options(const OAuthOptions &options);
Result<OAuthDiscovery> oauth_discovery(std::span<const std::byte> json, const OAuthOptions &options);
Result<bool> oauth_offered(std::span<const std::byte> mechanisms);

class OAuthExchange {
  enum class State {
    initial,
    awaiting,
    rejected,
    accepted
  };
  State state_ = State::initial;
  Result<SecretWriter> initial(std::span<const std::byte> mechanisms, const OAuthToken *token);

public:
  Result<SecretWriter> start(std::span<const std::byte> mechanisms, const OAuthToken &token);
  Result<SecretWriter> discover(std::span<const std::byte> mechanisms);
  Result<SecretWriter> reject();
  Result<void> accept();
  bool complete() const noexcept;
};

} // namespace weave::pg::detail
