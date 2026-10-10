#pragma once

#include "secret.hpp"
#include <weave/tls/context.hpp>
#include <string>
#include <string_view>

namespace weave::pg::detail {

struct OAuthHttpsUrl {
  std::string host;
  std::string authority;
  std::string target;
  u16 port = 443;
};

struct OAuthHttpRequest {
  std::string url;
  bool post = false;
  SecretText body;
  SecretText authorization;
};

struct OAuthHttpResponse {
  u16 status = 0;
  SecretText body;
};

Result<OAuthHttpsUrl> oauth_https_url(std::string_view url, bool allow_fragment = false);
Task<OAuthHttpResponse> oauth_https(
  OAuthHttpRequest request,
  TlsContext credentials,
  std::chrono::milliseconds deadline);

} // namespace weave::pg::detail
