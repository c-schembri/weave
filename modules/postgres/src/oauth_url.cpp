#include "oauth_http.hpp"
#include "oauth_uri_prefix.h"
#include "vendor/uriparser/include/uriparser/Uri.h"
#include <weave/address.hpp>
#include <algorithm>
#include <charconv>

namespace weave::pg::detail {

namespace {

struct ParsedUri {
  UriUriA value{};

  ~ParsedUri()
  {
    uriFreeUriMembersA(&value);
  }
};

std::string_view text(UriTextRangeA range) noexcept
{
  return range.first ? std::string_view{range.first, range.afterLast} : std::string_view{};
}

bool host_character(unsigned char byte) noexcept
{
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' ||
    byte == '.';
}

bool dns_name(std::string_view host) noexcept
{
  if (host.ends_with('.'))
    host.remove_suffix(1);
  while (!host.empty()) {
    auto end = host.find('.');
    auto label = host.substr(0, end);
    if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
      return false;
    if (end == std::string_view::npos)
      return true;
    host.remove_prefix(end + 1);
  }
  return false;
}

std::error_code invalid() noexcept
{
  return std::make_error_code(std::errc::invalid_argument);
}

} // namespace

Result<OAuthHttpsUrl> oauth_https_url(std::string_view url, bool allow_fragment)
{
  if (url.empty() || url.size() > 8192 || !std::ranges::all_of(url, [](unsigned char byte) {
        return byte >= 0x21 && byte <= 0x7e;
      }))
    return std::unexpected(invalid());

  ParsedUri parsed;
  auto result = uriParseSingleUriExA(&parsed.value, url.data(), url.data() + url.size(), nullptr);
  if (result != URI_SUCCESS) {
    auto error = result == URI_ERROR_MALLOC ? std::make_error_code(std::errc::not_enough_memory) : invalid();
    return std::unexpected(error);
  }
  const auto &uri = parsed.value;
  if (text(uri.scheme) != "https" || uri.userInfo.first || (!allow_fragment && uri.fragment.first) ||
    uri.hostData.ipFuture.first)
    return std::unexpected(invalid());

  auto host = text(uri.hostText);
  if (host.empty() || host.size() > 253)
    return std::unexpected(invalid());
  if (uri.hostData.ip6) {
    auto numeric = IpAddress::parse(host);
    if (!numeric || numeric->family() != AddressFamily::v6)
      return std::unexpected(invalid());
  } else {
    if (!std::ranges::all_of(host, host_character))
      return std::unexpected(invalid());
    bool numeric_spelling = std::ranges::all_of(host, [](unsigned char byte) {
      return (byte >= '0' && byte <= '9') || byte == '.';
    });
    if (numeric_spelling && !uri.hostData.ip4)
      return std::unexpected(invalid());
    if (!uri.hostData.ip4 && !dns_name(host))
      return std::unexpected(invalid());
  }

  OAuthHttpsUrl endpoint;
  endpoint.host = host;
  endpoint.authority = uri.hostData.ip6 ? "[" + std::string{host} + "]" : std::string{host};
  if (uri.portText.first) {
    auto port = text(uri.portText);
    auto converted = std::from_chars(port.data(), port.data() + port.size(), endpoint.port);
    if (converted.ec != std::errc{} || converted.ptr != port.data() + port.size() || !endpoint.port)
      return std::unexpected(invalid());
    endpoint.authority += ':';
    endpoint.authority += port;
  }

  // Use parsed encoded ranges verbatim; normalization could change issuer identity.
  endpoint.target = "/";
  for (auto *segment = uri.pathHead; segment; segment = segment->next) {
    endpoint.target += text(segment->text);
    if (segment->next)
      endpoint.target += '/';
  }
  if (uri.query.first) {
    endpoint.target += '?';
    endpoint.target += text(uri.query);
  }
  return endpoint;
}

} // namespace weave::pg::detail
