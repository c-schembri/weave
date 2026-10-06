#pragma once

#include <weave/core.hpp>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace weave {

enum class AddressFamily {
  any,
  v4,
  v6
};

class IpAddress {
public:
  constexpr IpAddress() noexcept = default;
  static Result<IpAddress> parse(std::string_view literal) noexcept;

  static constexpr IpAddress v4(std::array<u8, 4> bytes) noexcept
  {
    IpAddress address;
    for (std::size_t i = 0; i < bytes.size(); ++i)
      address.bytes_[i] = bytes[i];
    return address;
  }

  static constexpr IpAddress v6(std::array<u8, 16> bytes, u32 scope_id = 0) noexcept
  {
    IpAddress address;
    address.family_ = AddressFamily::v6;
    address.bytes_ = bytes;
    address.scope_id_ = scope_id;
    return address;
  }

  static constexpr IpAddress any_v4() noexcept
  {
    return {};
  }

  static constexpr IpAddress any_v6() noexcept
  {
    return v6({});
  }

  static constexpr IpAddress loopback_v4() noexcept
  {
    return v4({127, 0, 0, 1});
  }

  static constexpr IpAddress loopback_v6() noexcept
  {
    std::array<u8, 16> bytes{};
    bytes.back() = 1;
    return v6(bytes);
  }

  constexpr AddressFamily family() const noexcept
  {
    return family_;
  }

  constexpr bool is_v4() const noexcept
  {
    return family_ == AddressFamily::v4;
  }

  constexpr bool is_v6() const noexcept
  {
    return family_ == AddressFamily::v6;
  }

  constexpr u32 scope_id() const noexcept
  {
    return scope_id_;
  }

  constexpr std::span<const u8> bytes() const noexcept
  {
    return {bytes_.data(), is_v4() ? 4u : 16u};
  }

  std::string to_string() const;
  constexpr bool operator==(const IpAddress &) const noexcept = default;

private:
  std::array<u8, 16> bytes_{};
  u32 scope_id_ = 0;
  AddressFamily family_ = AddressFamily::v4;
};

struct Endpoint {
  IpAddress address;
  u16 port = 0;

  static Result<Endpoint> parse(std::string_view literal, u16 port) noexcept;
  std::string to_string() const;
  constexpr bool operator==(const Endpoint &) const noexcept = default;
};

} // namespace weave
