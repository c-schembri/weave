#include "ldap.hpp"
#include "options.hpp"
#include "oauth_uri_prefix.h"
#include "vendor/uriparser/include/uriparser/Uri.h"
#include <weave/address.hpp>
#include <algorithm>
#include <array>
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

std::string_view text(UriTextRangeA range)
{
  return range.first ? std::string_view{range.first, range.afterLast} : std::string_view{};
}

std::error_code invalid()
{
  return std::make_error_code(std::errc::invalid_argument);
}

bool equal_ascii(std::string_view value, std::string_view expected)
{
  return std::ranges::equal(value, expected, [](unsigned char left, unsigned char right) {
    if (left >= 'A' && left <= 'Z')
      left += 'a' - 'A';
    return left == right;
  });
}

Result<std::string> decoded(std::string_view value)
{
  auto bytes = decode_option_bytes(value);
  if (!bytes)
    return std::unexpected(bytes.error());
  return std::string{bytes->data(), bytes->size()};
}

bool dns_host(std::string_view host)
{
  if (host.ends_with('.'))
    host.remove_suffix(1);
  while (!host.empty()) {
    auto end = host.find('.');
    auto label = host.substr(0, end);
    bool characters = std::ranges::all_of(label, [](unsigned char byte) {
      return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
        byte == '-';
    });
    if (label.empty() || label.size() > 63 || !characters || label.front() == '-' || label.back() == '-')
      return false;
    if (end == std::string_view::npos)
      return true;
    host.remove_prefix(end + 1);
  }
  return false;
}

} // namespace

Result<LdapQuery> ldap_url(std::string_view url)
{
  if (url.empty() || url.size() > maximum_connection_field || !std::ranges::all_of(url, [](unsigned char byte) {
        return byte >= 0x21 && byte <= 0x7e;
      }))
    return std::unexpected(invalid());

  ParsedUri parsed;
  auto code = uriParseSingleUriExA(&parsed.value, url.data(), url.data() + url.size(), nullptr);
  if (code != URI_SUCCESS)
    return std::unexpected(code == URI_ERROR_MALLOC ? std::make_error_code(std::errc::not_enough_memory) : invalid());
  const auto &uri = parsed.value;
  if (!equal_ascii(text(uri.scheme), "ldap") || uri.userInfo.first || uri.fragment.first ||
    uri.hostData.ipFuture.first || !uri.hostText.first || !uri.query.first)
    return std::unexpected(invalid());

  LdapQuery query;
  auto host = text(uri.hostText);
  query.host = host.empty() ? "localhost" : host;
  query.ipv6 = uri.hostData.ip6 != nullptr;
  if (query.ipv6) {
    auto address = IpAddress::parse(host);
    if (!address || address->family() != AddressFamily::v6)
      return std::unexpected(invalid());
  } else if (query.host.size() > 253 || !dns_host(query.host)) {
    return std::unexpected(invalid());
  }
  bool numeric_host = std::ranges::all_of(query.host, [](unsigned char byte) {
    return (byte >= '0' && byte <= '9') || byte == '.';
  });
  if (numeric_host && !uri.hostData.ip4)
    return std::unexpected(invalid());
  if (uri.portText.first) {
    auto port = text(uri.portText);
    auto value = std::from_chars(port.data(), port.data() + port.size(), query.port);
    if (value.ec != std::errc{} || value.ptr != port.data() + port.size() || !query.port)
      return std::unexpected(invalid());
  }

  std::string base;
  for (auto *segment = uri.pathHead; segment; segment = segment->next) {
    base += text(segment->text);
    if (segment->next)
      base += '/';
  }
  auto decoded_base = decoded(base);
  if (!decoded_base || decoded_base->empty())
    return std::unexpected(decoded_base ? invalid() : decoded_base.error());
  query.base = std::move(*decoded_base);

  std::array<std::string_view, 3> parts;
  auto remaining = text(uri.query);
  for (std::size_t index = 0; index < parts.size(); ++index) {
    auto end = remaining.find('?');
    parts[index] = remaining.substr(0, end);
    if (index + 1 < parts.size() && end == std::string_view::npos)
      return std::unexpected(invalid());
    if (index + 1 == parts.size() && end != std::string_view::npos)
      return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end + 1);
  }

  auto attribute = decoded(parts[0]);
  auto filter = decoded(parts[2]);
  if (!attribute || !filter)
    return std::unexpected(attribute ? filter.error() : attribute.error());
  if (attribute->empty() || attribute->find(',') != std::string::npos || filter->empty())
    return std::unexpected(invalid());
  query.attribute = std::move(*attribute);
  query.filter = std::move(*filter);
  if (equal_ascii(parts[1], "base"))
    query.scope = LdapScope::base;
  else if (equal_ascii(parts[1], "one"))
    query.scope = LdapScope::one;
  else if (equal_ascii(parts[1], "sub"))
    query.scope = LdapScope::sub;
  else
    return std::unexpected(invalid());
  return query;
}

} // namespace weave::pg::detail
