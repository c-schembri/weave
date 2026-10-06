#include "address.hpp"
#include <weave/task.hpp>
#include <algorithm>
#include <charconv>
#include <cstring>

namespace weave {

Result<IpAddress> IpAddress::parse(std::string_view literal) noexcept
{
  auto invalid = [] { return std::unexpected(std::make_error_code(std::errc::invalid_argument)); };
  if (literal.empty() || literal.size() >= 64 || literal.find('\0') != std::string_view::npos)
    return invalid();

  u32 scope = 0;
  auto percent = literal.find('%');
  if (percent != std::string_view::npos) {
    const auto zone = literal.substr(percent + 1);
    auto parsed = std::from_chars(zone.data(), zone.data() + zone.size(), scope);
    if (zone.empty() || parsed.ec != std::errc{} || parsed.ptr != zone.data() + zone.size())
      return invalid();
    literal = literal.substr(0, percent);
    if (literal.find(':') == std::string_view::npos)
      return invalid();
  }

  std::array<char, 64> text{};
  std::copy(literal.begin(), literal.end(), text.begin());
  if (literal.find(':') != std::string_view::npos) {
    std::array<u8, 16> bytes{};
    if (InetPtonA(AF_INET6, text.data(), bytes.data()) != 1)
      return invalid();
    return v6(bytes, scope);
  }

  std::array<u8, 4> bytes{};
  if (InetPtonA(AF_INET, text.data(), bytes.data()) != 1)
    return invalid();
  return v4(bytes);
}

std::string IpAddress::to_string() const
{
  std::array<char, INET6_ADDRSTRLEN> text{};
  auto formatted = InetNtopA(is_v4() ? AF_INET : AF_INET6, bytes_.data(), text.data(), text.size());
  detail::require(formatted != nullptr);
  std::string result(text.data());
  if (scope_id_)
    result += '%' + std::to_string(scope_id_);
  return result;
}

Result<Endpoint> Endpoint::parse(std::string_view literal, u16 port) noexcept
{
  auto address = IpAddress::parse(literal);
  if (!address)
    return std::unexpected(address.error());
  return Endpoint{*address, port};
}

std::string Endpoint::to_string() const
{
  auto text = address.to_string();
  if (address.is_v6())
    text = '[' + text + ']';
  return text + ':' + std::to_string(port);
}

detail::SocketAddress detail::socket_address(const Endpoint &endpoint) noexcept
{
  SocketAddress result;
  if (endpoint.address.is_v4()) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    std::memcpy(&address.sin_addr, endpoint.address.bytes().data(), sizeof(address.sin_addr));
    std::memcpy(&result.storage, &address, sizeof(address));
    result.size = sizeof(address);
  } else {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(endpoint.port);
    address.sin6_scope_id = endpoint.address.scope_id();
    std::memcpy(&address.sin6_addr, endpoint.address.bytes().data(), sizeof(address.sin6_addr));
    std::memcpy(&result.storage, &address, sizeof(address));
    result.size = sizeof(address);
  }
  return result;
}

Result<Endpoint> detail::socket_endpoint(const sockaddr *address, std::size_t size) noexcept
{
  if (!address || size < sizeof(address->sa_family))
    return std::unexpected(std::make_error_code(std::errc::address_family_not_supported));
  if (address && address->sa_family == AF_INET && size >= sizeof(sockaddr_in)) {
    sockaddr_in native{};
    std::memcpy(&native, address, sizeof(native));
    std::array<u8, 4> bytes{};
    std::memcpy(bytes.data(), &native.sin_addr, bytes.size());
    return Endpoint{IpAddress::v4(bytes), ntohs(native.sin_port)};
  }
  if (address && address->sa_family == AF_INET6 && size >= sizeof(sockaddr_in6)) {
    sockaddr_in6 native{};
    std::memcpy(&native, address, sizeof(native));
    std::array<u8, 16> bytes{};
    std::memcpy(bytes.data(), &native.sin6_addr, bytes.size());
    return Endpoint{IpAddress::v6(bytes, native.sin6_scope_id), ntohs(native.sin6_port)};
  }
  return std::unexpected(std::make_error_code(std::errc::address_family_not_supported));
}

} // namespace weave
