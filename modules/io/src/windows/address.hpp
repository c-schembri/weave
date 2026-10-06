#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <weave/address.hpp>

namespace weave::detail {

// Shared native endpoint conversion for transports and name resolution.
struct SocketAddress {
  sockaddr_storage storage{};
  int size = 0;

  sockaddr *data() noexcept
  {
    return reinterpret_cast<sockaddr *>(&storage);
  }

  const sockaddr *data() const noexcept
  {
    return reinterpret_cast<const sockaddr *>(&storage);
  }
};

SocketAddress socket_address(const Endpoint &endpoint) noexcept;
Result<Endpoint> socket_endpoint(const sockaddr *address, std::size_t size) noexcept;

} // namespace weave::detail
