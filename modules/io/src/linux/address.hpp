#pragma once

#include <weave/address.hpp>
#include <arpa/inet.h>
#include <sys/socket.h>

namespace weave::detail {

struct SocketAddress {
  sockaddr_storage storage{};
  socklen_t size = 0;

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
