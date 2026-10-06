#pragma once
#include "native_errors.hpp"
#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

// Identical blocking peer for both benchmarks. Not part of the Weave runtime.
namespace support {

#if defined(_WIN32)
using Socket = SOCKET;
using SocketLength = int;
inline constexpr Socket invalid_socket = INVALID_SOCKET;
#else
using Socket = int;
using SocketLength = socklen_t;
inline constexpr Socket invalid_socket = -1;
#endif

inline int close_socket(Socket socket)
{
#if defined(_WIN32)
  return closesocket(socket);
#else
  return ::close(socket);
#endif
}

inline void check(bool ok)
{
  if (!ok) {
#if defined(_WIN32)
    const auto error = WSAGetLastError();
#else
    const auto error = errno;
#endif
    std::fprintf(stderr, "Loopback fixture failed: %d\n", error);
    std::abort();
  }
}

inline void configure(Socket socket)
{
#if defined(_WIN32)
  DWORD timeout = 5000;
  BOOL no_delay = TRUE;
#else
  timeval timeout{5, 0};
  int no_delay = 1;
#endif
  check(setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout)) == 0);
  check(setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char *>(&timeout), sizeof(timeout)) == 0);
  check(setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&no_delay), sizeof(no_delay)) == 0);
}

inline Socket connect(std::uint16_t port)
{
  auto socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  check(socket != invalid_socket);
  configure(socket);
  sockaddr_in endpoint{};
  endpoint.sin_family = AF_INET;
  endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  endpoint.sin_port = htons(port);
  check(::connect(socket, reinterpret_cast<sockaddr *>(&endpoint), sizeof(endpoint)) == 0);
  return socket;
}

inline bool write_all(Socket socket, const char *data, std::size_t size)
{
  while (size) {
#if defined(_WIN32)
    constexpr int flags = 0;
#else
    constexpr int flags = MSG_NOSIGNAL;
#endif
    int n = send(socket, data, static_cast<int>((std::min)(size, std::size_t{65536})), flags);
    if (n <= 0)
      return false;
    data += n;
    size -= n;
  }
  return true;
}

inline bool read_exactly(Socket socket, char *data, std::size_t size)
{
  while (size) {
    int n = recv(socket, data, static_cast<int>((std::min)(size, std::size_t{65536})), 0);
    if (n <= 0)
      return false;
    data += n;
    size -= n;
  }
  return true;
}

class EchoPeer {
public:
  explicit EchoPeer(std::size_t chunk = 65536)
  {
#if defined(_WIN32)
    WSADATA data{};
    check(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    check(listener_ != invalid_socket);
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(bind(listener_, reinterpret_cast<sockaddr *>(&endpoint), sizeof(endpoint)) == 0);
    check(listen(listener_, 1) == 0);
    SocketLength length = sizeof(endpoint);
    check(getsockname(listener_, reinterpret_cast<sockaddr *>(&endpoint), &length) == 0);
    port_ = ntohs(endpoint.sin_port);
    thread_ = std::thread([this, chunk] {
      auto client = accept(listener_, nullptr, nullptr);
      if (client == invalid_socket)
        return;
      configure(client);
      std::vector<char> buffer(chunk);
      for (;;) {
        int n = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (n == 0)
          break;
        if (n < 0 || !write_all(client, buffer.data(), static_cast<std::size_t>(n))) {
          ok_ = false;
          break;
        }
      }
      close_socket(client);
    });
  }

  ~EchoPeer()
  {
    close_socket(listener_);
    if (thread_.joinable())
      thread_.join();
#if defined(_WIN32)
    WSACleanup();
#endif
  }

  void join()
  {
    if (thread_.joinable())
      thread_.join();
  }

  bool ok() const
  {
    return ok_;
  }

  std::uint16_t port() const
  {
    return port_;
  }

private:
  Socket listener_ = invalid_socket;
  std::uint16_t port_ = 0;
  std::thread thread_;
  std::atomic<bool> ok_ = true;
};

} // namespace support
