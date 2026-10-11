#pragma once

#include <weave/core.hpp>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace weave {

/// Address selection for resolution; an IpAddress itself is always v4 or v6.
enum class AddressFamily {
  any,
  v4,
  v6
};

/// Owning numeric IP value, including an IPv6 scope ID. No DNS or native handles.
class IpAddress {
public:
  /// Construct the IPv4 wildcard address 0.0.0.0.
  constexpr IpAddress() noexcept = default;
  /// Parse a numeric literal synchronously; borrows text only during this call and never resolves DNS.
  static Result<IpAddress> parse(std::string_view literal) noexcept;

  /// Construct from four network-order octets, copied into the value.
  static constexpr IpAddress v4(std::array<u8, 4> bytes) noexcept
  {
    IpAddress address;
    for (std::size_t i = 0; i < bytes.size(); ++i)
      address.bytes_[i] = bytes[i];
    return address;
  }

  /// Construct from sixteen network-order octets and an optional interface scope ID.
  static constexpr IpAddress v6(std::array<u8, 16> bytes, u32 scope_id = 0) noexcept
  {
    IpAddress address;
    address.family_ = AddressFamily::v6;
    address.bytes_ = bytes;
    address.scope_id_ = scope_id;
    return address;
  }

  /// IPv4 wildcard for binding all local interfaces.
  static constexpr IpAddress any_v4() noexcept
  {
    return {};
  }

  /// IPv6 wildcard; listener dual-stack policy is configured separately.
  static constexpr IpAddress any_v6() noexcept
  {
    return v6({});
  }

  /// IPv4 loopback 127.0.0.1.
  static constexpr IpAddress loopback_v4() noexcept
  {
    return v4({127, 0, 0, 1});
  }

  /// IPv6 loopback ::1.
  static constexpr IpAddress loopback_v6() noexcept
  {
    std::array<u8, 16> bytes{};
    bytes.back() = 1;
    return v6(bytes);
  }

  /// The stored address family, never any.
  constexpr AddressFamily family() const noexcept
  {
    return family_;
  }

  /// Whether this value holds an IPv4 address.
  constexpr bool is_v4() const noexcept
  {
    return family_ == AddressFamily::v4;
  }

  /// Whether this value holds an IPv6 address.
  constexpr bool is_v6() const noexcept
  {
    return family_ == AddressFamily::v6;
  }

  /// IPv6 interface scope ID, or zero when absent.
  constexpr u32 scope_id() const noexcept
  {
    return scope_id_;
  }

  /// Borrow four or sixteen network-order octets; invalidated when this value dies or changes.
  constexpr std::span<const u8> bytes() const noexcept
  {
    return {bytes_.data(), is_v4() ? 4u : 16u};
  }

  /// Allocate an owning numeric representation, including the IPv6 scope when present.
  std::string to_string() const;
  constexpr bool operator==(const IpAddress &) const noexcept = default;

private:
  std::array<u8, 16> bytes_{};
  u32 scope_id_ = 0;
  AddressFamily family_ = AddressFamily::v4;
};

/// Owning numeric address and host-order port. Port zero requests an OS-chosen listening port.
struct Endpoint {
  IpAddress address;
  u16 port = 0;

  /// Parse an IP literal and copy the port; input is borrowed only during this synchronous call.
  static Result<Endpoint> parse(std::string_view literal, u16 port) noexcept;
  /// Allocate an address:port representation, with brackets around IPv6 addresses.
  std::string to_string() const;
  constexpr bool operator==(const Endpoint &) const noexcept = default;
};

} // namespace weave
