#pragma once

#include <weave/local.hpp>
#if defined(_WIN32)
#include "windows/address.hpp"
#include <afunix.h>
#else
#include "linux/address.hpp"
#include <sys/un.h>
#endif
#include <cstring>

namespace weave::detail {

inline Result<SocketAddress> local_socket_address(const std::string &address)
{
  sockaddr_un native{};
  if (address.empty() || address.find('\0') != std::string::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  bool abstract = address.front() == '@';
  auto bytes = address.size() + (abstract ? 0 : 1);
  if (bytes > sizeof(native.sun_path))
    return std::unexpected(std::make_error_code(std::errc::filename_too_long));
  if (abstract && address.size() == 1)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  native.sun_family = AF_UNIX;
  std::memcpy(native.sun_path, address.data(), address.size());
  if (abstract)
    native.sun_path[0] = '\0';

  SocketAddress endpoint;
  static_assert(sizeof(native) <= sizeof(endpoint.storage));
  std::memcpy(&endpoint.storage, &native, sizeof(native));
#if defined(_WIN32)
  endpoint.size = sizeof(native);
#else
  endpoint.size = static_cast<decltype(endpoint.size)>(offsetof(sockaddr_un, sun_path) + bytes);
#endif
  return endpoint;
}

inline Result<std::string> local_socket_name(const sockaddr_un &address, std::size_t size)
{
  constexpr auto offset = offsetof(sockaddr_un, sun_path);
  if (address.sun_family != AF_UNIX || size < offset || size > sizeof(address))
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (size == offset)
    return std::string{};

  auto bytes = size - offset;
  if (address.sun_path[0] == '\0') {
#if defined(_WIN32)
    // Winsock returns a full, zero-padded sockaddr_un even for unnamed peers.
    while (bytes != 0 && address.sun_path[bytes - 1] == '\0')
      --bytes;
    if (bytes == 0)
      return std::string{};
#endif
    std::string name(address.sun_path, bytes);
    name[0] = '@';
    return name;
  }
  auto end = static_cast<const char *>(std::memchr(address.sun_path, '\0', bytes));
  if (!end)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  return std::string(address.sun_path, static_cast<std::size_t>(end - address.sun_path));
}

} // namespace weave::detail
