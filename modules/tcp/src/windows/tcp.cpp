#include <winsock2.h>
#include <weave/tcp.hpp>
#include "windows/iocp.hpp"
#include "windows/address.hpp"
#include "windows/socket.hpp"
#include "../socket_options.hpp"
#include <weave/resolve.hpp>
#include <weave/io/detail/trace.hpp>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <mutex>
#include <type_traits>
#if defined(WEAVE_PROFILE_RUNTIME)
#include <chrono>
#endif

namespace weave {

namespace {

Error win_error(int code)
{
  return {code, std::system_category()};
}

Error last_error()
{
  return win_error(WSAGetLastError());
}

Error invalid()
{
  return std::make_error_code(std::errc::invalid_argument);
}

Error busy()
{
  return std::make_error_code(std::errc::operation_in_progress);
}

Result<void> socket_option(SOCKET socket, int level, int option, DWORD value)
{
  if (setsockopt(socket, level, option, reinterpret_cast<const char *>(&value), sizeof(value)) != 0)
    return std::unexpected(last_error());
  return {};
}

Result<DWORD> socket_option(SOCKET socket, int level, int option)
{
  DWORD value = 0;
  int size = sizeof(value);
  if (getsockopt(socket, level, option, reinterpret_cast<char *>(&value), &size) != 0)
    return std::unexpected(last_error());
  return value;
}

template <class T>
Result<T> extension(SOCKET socket, GUID id)
{
  T function = nullptr;
  DWORD bytes = 0;

  auto result = WSAIoctl(
    socket,
    SIO_GET_EXTENSION_FUNCTION_POINTER,
    &id,
    sizeof(id),
    &function,
    sizeof(function),
    &bytes,
    nullptr,
    nullptr);

  if (result != 0)
    return std::unexpected(last_error());

  return function;
}

struct TcpOperationFlag {
  bool &value;

  explicit TcpOperationFlag(bool &flag) : value(flag)
  {
    value = true;
  }

  ~TcpOperationFlag()
  {
    value = false;
  }
};

using TcpIoAwaiter = detail::SocketIoAwaiter<>;

Result<SOCKET> make_socket(Context &context, bool &skip_success, int family)
{
  detail::IoAccess::check_thread(context);
  if (context.stop_requested())
    return std::unexpected(win_error(ERROR_OPERATION_ABORTED));

  // Each socket owns one Winsock reference, including across moves and worker migration.
  WSADATA data{};
  if (auto code = WSAStartup(MAKEWORD(2, 2), &data); code != 0)
    return std::unexpected(win_error(code));

  SOCKET socket = WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
  if (socket == INVALID_SOCKET) {
    auto error = last_error();
    WSACleanup();
    return std::unexpected(error);
  }

  bool supports_skipping = false;
  if (detail::IoAccess::state(context).options_.skip_successful_completions) {
    WSAPROTOCOL_INFOW protocol{};
    int protocol_size = sizeof(protocol);
    auto *protocol_data = reinterpret_cast<char *>(&protocol);
    auto queried = getsockopt(socket, SOL_SOCKET, SO_PROTOCOL_INFOW, protocol_data, &protocol_size);
    supports_skipping = queried == 0 && (protocol.dwServiceFlags1 & XP1_IFS_HANDLES) != 0;
  }

  auto attached = detail::IoAccess::attach(context, socket, supports_skipping);
  if (!attached) {
    auto error = attached.error();
    closesocket(socket);
    WSACleanup();
    return std::unexpected(error);
  }

  skip_success = *attached;
  return socket;
}

Result<void> close_socket(Context &context, SOCKET socket)
{
  return detail::IoAccess::close(context, socket, [](std::uintptr_t handle) noexcept -> Error {
    if (closesocket(handle) != 0)
      return last_error();
    WSACleanup();
    return {};
  });
}

} // namespace

Task<TcpListener> tcp::listen(const char *ipv4, u16 port, int backlog)
{
  auto *ctx = detail::current_context;
  detail::require(ctx != nullptr);
  auto listener = tcp::listen(*ctx, ipv4, port, backlog);
  if (!listener)
    co_await fail(listener.error());
  co_return std::move(*listener);
}

Task<TcpStream> tcp::connect(const char *ipv4, u16 port)
{
  auto *ctx = detail::current_context;
  detail::require(ctx != nullptr);
  co_return co_await tcp::connect(*ctx, ipv4, port);
}

Task<TcpListener> tcp::listen(Endpoint endpoint, ListenOptions options)
{
  auto *ctx = detail::current_context;
  detail::require(ctx != nullptr);
  auto listener = tcp::listen(*ctx, endpoint, options);
  if (!listener)
    co_await fail(listener.error());
  co_return std::move(*listener);
}

Result<TcpListener> tcp::listen(Context &context, const char *address, u16 port, int backlog)
{
  detail::IoAccess::check_thread(context);
  if (!address)
    return std::unexpected(invalid());
  auto endpoint = Endpoint::parse(address, port);
  if (!endpoint)
    return std::unexpected(endpoint.error());
  return tcp::listen(context, *endpoint, {.backlog = backlog});
}

Result<TcpListener> tcp::listen(Context &context, Endpoint endpoint, ListenOptions options)
{
  detail::IoAccess::check_thread(context);
  auto native = detail::socket_address(endpoint);

  bool skip_success = false;
  auto socket = make_socket(context, skip_success, native.data()->sa_family);
  if (!socket)
    return std::unexpected(socket.error());

  TcpListener listener(context, *socket, skip_success);
  BOOL exclusive = TRUE;
  auto configured = setsockopt(
    *socket,
    SOL_SOCKET,
    SO_EXCLUSIVEADDRUSE,
    reinterpret_cast<const char *>(&exclusive),
    sizeof(exclusive));
  if (configured != 0)
    return std::unexpected(last_error());

  if (endpoint.address.is_v6()) {
    DWORD only = options.ipv6_only;
    if (setsockopt(*socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&only), sizeof(only)) != 0)
      return std::unexpected(last_error());
  }

  auto bound = bind(*socket, native.data(), native.size);
  if (bound != 0)
    return std::unexpected(last_error());

  // Ordinary positive backlogs can be silently capped at 200 by Winsock.
  auto native_backlog = options.backlog;
  if (options.backlog > 200 && options.backlog != SOMAXCONN)
    native_backlog = SOMAXCONN_HINT((std::min)(options.backlog, 65535));
  auto listening = ::listen(*socket, native_backlog);
  if (listening != 0)
    return std::unexpected(last_error());

  sockaddr_storage local{};
  int size = sizeof(local);
  if (getsockname(*socket, reinterpret_cast<sockaddr *>(&local), &size) != 0)
    return std::unexpected(last_error());
  auto bound_endpoint = detail::socket_endpoint(reinterpret_cast<sockaddr *>(&local), size);
  if (!bound_endpoint)
    return std::unexpected(bound_endpoint.error());
  listener.local_ = *bound_endpoint;

  return listener;
}

Task<TcpStream> tcp::connect(Context &context, const char *host, u16 port)
{
  detail::IoAccess::check_execution(context);
  if (!host)
    co_await fail(invalid());
  auto numeric = Endpoint::parse(host, port);
  if (numeric)
    co_return co_await tcp::connect(context, *numeric);
  auto endpoints = co_await resolve(context, std::string(host), port);
  co_return co_await tcp::connect(context, std::move(endpoints));
}

Task<TcpStream> tcp::connect(Context &context, std::string host, u16 port)
{
  detail::IoAccess::check_execution(context);
  if (host.find('\0') != std::string::npos)
    co_await fail(invalid());
  co_return co_await tcp::connect(context, host.c_str(), port);
}

Task<TcpStream> tcp::connect(std::string host, u16 port)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  co_return co_await tcp::connect(*context, std::move(host), port);
}

Task<TcpStream> tcp::connect(Endpoint endpoint)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  co_return co_await tcp::connect(*context, endpoint);
}

Task<TcpStream> tcp::connect(std::vector<Endpoint> endpoints)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  co_return co_await tcp::connect(*context, std::move(endpoints));
}

Task<TcpStream> tcp::connect(Context &context, std::vector<Endpoint> endpoints)
{
  detail::IoAccess::check_execution(context);
  if (endpoints.empty())
    co_await fail(invalid());
  Error error;
  for (auto endpoint : endpoints) {
    co_await cancellation_point();
    if (context.stop_requested())
      co_await fail(std::errc::operation_canceled);
    auto connected = co_await as_result(tcp::connect(context, endpoint));
    if (connected)
      co_return std::move(*connected);
    error = connected.error();
    if (error == std::errc::operation_canceled)
      co_await fail(error);
  }
  co_await fail(error);
}

Task<TcpStream> tcp::connect(Context &context, Endpoint endpoint)
{
  detail::IoAccess::check_execution(context);
  co_await cancellation_point();
  if (context.stop_requested())
    co_await fail(std::errc::operation_canceled);
  auto native = detail::socket_address(endpoint);

  bool skip_success = false;
  auto socket = make_socket(context, skip_success, native.data()->sa_family);
  if (!socket)
    co_return std::unexpected(socket.error());

  TcpStream stream(context, *socket, skip_success);
  auto local = detail::socket_address({endpoint.address.is_v4() ? IpAddress::any_v4() : IpAddress::any_v6(), 0});
  auto bound = bind(*socket, local.data(), local.size);
  if (bound != 0)
    co_return std::unexpected(last_error());

  auto function = extension<LPFN_CONNECTEX>(*socket, WSAID_CONNECTEX);
  if (!function)
    co_return std::unexpected(function.error());

  TcpIoAwaiter operation(TcpIoAwaiter::connect, context, *socket, skip_success);
  operation.endpoint = &native;
  operation.connect_fn = *function;

  auto connected = co_await operation;
  if (!connected)
    co_return std::unexpected(connected.error());

  auto updated = setsockopt(*socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0);
  if (updated != 0)
    co_return std::unexpected(last_error());

  co_return std::move(stream);
}

TcpStream::TcpStream(TcpStream &&other) noexcept : ctx_(other.ctx_), socket_(INVALID_SOCKET)
{
  detail::require(!other.reading_ && !other.writing_);
  socket_ = std::exchange(other.socket_, INVALID_SOCKET);
  skip_success_ = other.skip_success_;
}

TcpStream::~TcpStream()
{
  detail::require(static_cast<bool>(close()));
}

Result<void> TcpStream::close()
{
  detail::IoAccess::check_thread(*ctx_);
  if (reading_ || writing_)
    return std::unexpected(busy());

  if (socket_ != INVALID_SOCKET) {
    auto closed = close_socket(*ctx_, socket_);
    if (!closed)
      return closed;

    socket_ = INVALID_SOCKET;
  }

  return {};
}

Result<void> TcpStream::no_delay(bool enabled)
{
  detail::IoAccess::check_thread(*ctx_);
  BOOL value = enabled;
  auto configured = setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&value), sizeof(value));
  if (configured != 0)
    return std::unexpected(last_error());

  return {};
}

Result<void> TcpStream::keep_alive(TcpKeepAliveOptions options)
{
  detail::IoAccess::check_thread(*ctx_);
  if (!detail::valid_keep_alive(options))
    return std::unexpected(invalid());

  if (auto result = socket_option(socket_, SOL_SOCKET, SO_KEEPALIVE, options.enabled); !result)
    return result;
  if (!options.enabled)
    return {};

  if (options.idle.count() != 0) {
    auto result = socket_option(socket_, IPPROTO_TCP, TCP_KEEPIDLE, static_cast<DWORD>(options.idle.count()));
    if (!result)
      return result;
  }
  if (options.interval.count() != 0) {
    auto result = socket_option(socket_, IPPROTO_TCP, TCP_KEEPINTVL, static_cast<DWORD>(options.interval.count()));
    if (!result)
      return result;
  }
  if (options.probes != 0) {
    auto result = socket_option(socket_, IPPROTO_TCP, TCP_KEEPCNT, static_cast<DWORD>(options.probes));
    if (!result)
      return result;
  }
  return {};
}

Result<TcpKeepAliveOptions> TcpStream::keep_alive_options() const
{
  detail::IoAccess::check_thread(*ctx_);
  auto enabled = socket_option(socket_, SOL_SOCKET, SO_KEEPALIVE);
  if (!enabled)
    return std::unexpected(enabled.error());
  auto idle = socket_option(socket_, IPPROTO_TCP, TCP_KEEPIDLE);
  if (!idle)
    return std::unexpected(idle.error());
  auto interval = socket_option(socket_, IPPROTO_TCP, TCP_KEEPINTVL);
  if (!interval)
    return std::unexpected(interval.error());
  auto probes = socket_option(socket_, IPPROTO_TCP, TCP_KEEPCNT);
  if (!probes)
    return std::unexpected(probes.error());

  return TcpKeepAliveOptions{
    .enabled = *enabled != 0,
    .idle = std::chrono::seconds{*idle},
    .interval = std::chrono::seconds{*interval},
    .probes = static_cast<u32>(*probes)};
}

Result<void> TcpStream::user_timeout(std::chrono::milliseconds timeout)
{
  detail::IoAccess::check_thread(*ctx_);
  if (!detail::valid_socket_timeout(timeout))
    return std::unexpected(invalid());
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
}

Result<std::chrono::milliseconds> TcpStream::user_timeout() const
{
  detail::IoAccess::check_thread(*ctx_);
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
}

Result<void> TcpStream::shutdown_send()
{
  detail::IoAccess::check_thread(*ctx_);
  if (writing_)
    return std::unexpected(busy());

  if (shutdown(socket_, SD_SEND) != 0)
    return std::unexpected(last_error());

  return {};
}

Result<Endpoint> TcpStream::local_endpoint() const
{
  detail::IoAccess::check_thread(*ctx_);
  sockaddr_storage address{};
  int size = sizeof(address);
  if (getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &size) != 0)
    return std::unexpected(last_error());
  return detail::socket_endpoint(reinterpret_cast<sockaddr *>(&address), size);
}

Result<Endpoint> TcpStream::peer_endpoint() const
{
  detail::IoAccess::check_thread(*ctx_);
  sockaddr_storage address{};
  int size = sizeof(address);
  if (getpeername(socket_, reinterpret_cast<sockaddr *>(&address), &size) != 0)
    return std::unexpected(last_error());
  return detail::socket_endpoint(reinterpret_cast<sockaddr *>(&address), size);
}

Result<void> TcpStream::cancel()
{
  detail::IoAccess::check_thread(*ctx_);
  if (socket_ == INVALID_SOCKET)
    return std::unexpected(win_error(WSAENOTSOCK));

  if (!CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr)) {
    auto error = GetLastError();
    if (error != ERROR_NOT_FOUND)
      return std::unexpected(win_error(static_cast<int>(error)));
  }
  return {};
}

Task<std::size_t> TcpStream::read(std::span<std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (reading_)
    co_return std::unexpected(busy());

  if (socket_ == INVALID_SOCKET)
    co_return std::unexpected(win_error(WSAENOTSOCK));

  if (buffer.empty())
    co_return std::size_t{0};

  TcpOperationFlag guard(reading_);
  TcpIoAwaiter operation(TcpIoAwaiter::receive, *ctx_, socket_, skip_success_);
  operation.buffer.buf = reinterpret_cast<char *>(buffer.data());
  operation.buffer.len = static_cast<ULONG>((std::min)(buffer.size(), std::size_t{65536}));

  auto result = co_await operation;
  if (!result)
    co_await fail(result.error());
  co_return *result;
}

Task<void> TcpStream::read_exactly(std::span<std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (reading_)
    co_await fail(busy());

  if (socket_ == INVALID_SOCKET)
    co_await fail(win_error(WSAENOTSOCK));

  TcpOperationFlag guard(reading_);
  while (!buffer.empty()) {
    TcpIoAwaiter operation(TcpIoAwaiter::receive, *ctx_, socket_, skip_success_);
    operation.buffer.buf = reinterpret_cast<char *>(buffer.data());
    operation.buffer.len = static_cast<ULONG>((std::min)(buffer.size(), std::size_t{65536}));

    auto n = co_await operation;
    if (!n)
      co_await fail(n.error());
    if (*n == 0)
      co_await fail(std::errc::connection_reset);

    buffer = buffer.subspan(*n);
  }
  co_return;
}

Task<void> TcpStream::write_all(std::span<const std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (writing_)
    co_await fail(busy());

  if (socket_ == INVALID_SOCKET)
    co_await fail(win_error(WSAENOTSOCK));

  TcpOperationFlag guard(writing_);
  while (!buffer.empty()) {
    TcpIoAwaiter operation(TcpIoAwaiter::send, *ctx_, socket_, skip_success_);
    operation.buffer.buf = const_cast<char *>(reinterpret_cast<const char *>(buffer.data()));
    operation.buffer.len = static_cast<ULONG>((std::min)(buffer.size(), std::size_t{65536}));

    auto n = co_await operation;
    if (!n)
      co_await fail(n.error());
    if (*n == 0)
      co_await fail(std::errc::broken_pipe);

    buffer = buffer.subspan(*n);
  }
  co_return;
}

TcpListener::TcpListener(TcpListener &&other) noexcept : ctx_(other.ctx_), socket_(INVALID_SOCKET)
{
  detail::require(!other.accepting_);
  socket_ = std::exchange(other.socket_, INVALID_SOCKET);
  local_ = std::exchange(other.local_, Endpoint{});
  skip_success_ = other.skip_success_;
}

TcpListener::~TcpListener()
{
  detail::require(static_cast<bool>(close()));
}

Result<void> TcpListener::close()
{
  detail::IoAccess::check_thread(*ctx_);
  if (accepting_)
    return std::unexpected(busy());

  if (socket_ != INVALID_SOCKET) {
    auto closed = close_socket(*ctx_, socket_);
    if (!closed)
      return closed;
    socket_ = INVALID_SOCKET;
  }
  return {};
}

Result<void> TcpListener::cancel()
{
  detail::IoAccess::check_thread(*ctx_);
  if (socket_ == INVALID_SOCKET)
    return std::unexpected(win_error(WSAENOTSOCK));
  if (!CancelIoEx(reinterpret_cast<HANDLE>(socket_), nullptr)) {
    auto error = GetLastError();
    if (error != ERROR_NOT_FOUND)
      return std::unexpected(win_error(static_cast<int>(error)));
  }
  return {};
}

Task<TcpStream> TcpListener::accept(AcceptOptions options)
{
  detail::IoAccess::check_execution(*ctx_);
  if (accepting_)
    co_return std::unexpected(busy());

  TcpOperationFlag guard(accepting_);
  auto function = extension<LPFN_ACCEPTEX>(socket_, WSAID_ACCEPTEX);
  if (!function)
    co_return std::unexpected(function.error());

  bool skip_success = false;
  auto socket = make_socket(*ctx_, skip_success, local_.address.is_v4() ? AF_INET : AF_INET6);
  if (!socket)
    co_return std::unexpected(socket.error());

  TcpStream stream(*ctx_, *socket, skip_success);
  TcpIoAwaiter operation(TcpIoAwaiter::accept, *ctx_, socket_, skip_success_);
  operation.accepted = *socket;
  operation.accept_fn = *function;
  operation.address_bytes = local_.address.is_v4() ? sizeof(sockaddr_in) + 16 : sizeof(sockaddr_in6) + 16;

  auto accepted = co_await operation;
  if (!accepted)
    co_return std::unexpected(accepted.error());

  auto updated = setsockopt(
    *socket,
    SOL_SOCKET,
    SO_UPDATE_ACCEPT_CONTEXT,
    reinterpret_cast<const char *>(&socket_),
    sizeof(socket_));
  if (updated != 0)
    co_return std::unexpected(last_error());

  if (options.no_delay) {
    auto configured = stream.no_delay();
    if (!configured)
      co_return std::unexpected(configured.error());
  }

  co_return std::move(stream);
}

} // namespace weave
