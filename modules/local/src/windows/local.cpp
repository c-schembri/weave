#include <winsock2.h>
#include <weave/local.hpp>
#include "windows/socket.hpp"
#include "../address.hpp"
#include <algorithm>

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

Error busy()
{
  return std::make_error_code(std::errc::operation_in_progress);
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

struct LocalOperationFlag {
  bool &value;

  explicit LocalOperationFlag(bool &flag) : value(flag)
  {
    value = true;
  }

  ~LocalOperationFlag()
  {
    value = false;
  }
};

using LocalIoAwaiter = detail::SocketIoAwaiter<>;

Result<SOCKET> make_socket(Context &context, bool &skip_success)
{
  detail::IoAccess::check_thread(context);
  if (context.stop_requested())
    return std::unexpected(win_error(ERROR_OPERATION_ABORTED));

  // Each socket owns one Winsock reference, including across moves and worker migration.
  WSADATA data{};
  if (auto code = WSAStartup(MAKEWORD(2, 2), &data); code != 0)
    return std::unexpected(win_error(code));

  SOCKET socket = WSASocketW(AF_UNIX, SOCK_STREAM, 0, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
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

Task<LocalStream> local::connect(std::string address)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  co_return co_await local::connect(*context, std::move(address));
}

Task<LocalListener> local::listen(std::string address, int backlog)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  auto listener = local::listen(*context, std::move(address), backlog);
  if (!listener)
    co_await fail(listener.error());
  co_return std::move(*listener);
}

Task<LocalStream> local::connect(Context &context, std::string address)
{
  detail::IoAccess::check_execution(context);
  co_await cancellation_point();
  auto native = detail::local_socket_address(address);
  if (!native)
    co_await fail(native.error());
  if (address.front() == '@')
    co_await fail(std::errc::operation_not_supported);

  bool skip_success = false;
  auto socket = make_socket(context, skip_success);
  if (!socket)
    co_await fail(socket.error());
  LocalStream stream(context, *socket, skip_success);

  sockaddr_un local{};
  local.sun_family = AF_UNIX;
  if (bind(*socket, reinterpret_cast<sockaddr *>(&local), sizeof(sockaddr)) != 0)
    co_await fail(last_error());

  auto function = extension<LPFN_CONNECTEX>(*socket, WSAID_CONNECTEX);
  if (!function)
    co_await fail(function.error());
  LocalIoAwaiter operation(LocalIoAwaiter::connect, context, *socket, skip_success);
  operation.connect_fn = *function;
  operation.endpoint = &*native;
  auto connected = co_await operation;
  if (!connected)
    co_await fail(connected.error());

  if (setsockopt(*socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) != 0)
    co_await fail(last_error());

  co_return std::move(stream);
}

Result<LocalListener> local::listen(Context &context, std::string address, int backlog)
{
  detail::IoAccess::check_thread(context);
  if (backlog <= 0)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  auto native = detail::local_socket_address(address);
  if (!native)
    return std::unexpected(native.error());
  if (address.front() == '@')
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  bool skip_success = false;
  auto socket = make_socket(context, skip_success);
  if (!socket)
    return std::unexpected(socket.error());
  LocalListener listener(context, *socket, skip_success, std::move(address));
  if (bind(*socket, native->data(), native->size) != 0)
    return std::unexpected(last_error());

  auto native_backlog = backlog;
  if (backlog > 200 && backlog != SOMAXCONN)
    native_backlog = SOMAXCONN_HINT((std::min)(backlog, 65535));
  if (::listen(*socket, native_backlog) != 0)
    return std::unexpected(last_error());
  return listener;
}

LocalStream::LocalStream(LocalStream &&other) noexcept : ctx_(other.ctx_), socket_(INVALID_SOCKET)
{
  detail::require(!other.reading_ && !other.writing_);
  socket_ = std::exchange(other.socket_, INVALID_SOCKET);
  skip_success_ = other.skip_success_;
}

LocalStream::~LocalStream()
{
  detail::require(static_cast<bool>(close()));
}

Result<void> LocalStream::close()
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

Result<void> LocalStream::shutdown_send()
{
  detail::IoAccess::check_thread(*ctx_);
  if (writing_)
    return std::unexpected(busy());

  if (shutdown(socket_, SD_SEND) != 0)
    return std::unexpected(last_error());

  return {};
}

Result<void> LocalStream::cancel()
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

Task<std::size_t> LocalStream::read(std::span<std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (reading_)
    co_return std::unexpected(busy());

  if (socket_ == INVALID_SOCKET)
    co_return std::unexpected(win_error(WSAENOTSOCK));

  if (buffer.empty())
    co_return std::size_t{0};

  LocalOperationFlag guard(reading_);
  LocalIoAwaiter operation(LocalIoAwaiter::receive, *ctx_, socket_, skip_success_);
  operation.buffer.buf = reinterpret_cast<char *>(buffer.data());
  operation.buffer.len = static_cast<ULONG>((std::min)(buffer.size(), std::size_t{65536}));

  auto result = co_await operation;
  if (!result)
    co_await fail(result.error());
  co_return *result;
}

Task<void> LocalStream::read_exactly(std::span<std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (reading_)
    co_await fail(busy());

  if (socket_ == INVALID_SOCKET)
    co_await fail(win_error(WSAENOTSOCK));

  LocalOperationFlag guard(reading_);
  while (!buffer.empty()) {
    LocalIoAwaiter operation(LocalIoAwaiter::receive, *ctx_, socket_, skip_success_);
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

Task<void> LocalStream::write_all(std::span<const std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (writing_)
    co_await fail(busy());

  if (socket_ == INVALID_SOCKET)
    co_await fail(win_error(WSAENOTSOCK));

  LocalOperationFlag guard(writing_);
  while (!buffer.empty()) {
    LocalIoAwaiter operation(LocalIoAwaiter::send, *ctx_, socket_, skip_success_);
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

Result<std::string> LocalStream::local_address() const
{
  detail::IoAccess::check_thread(*ctx_);
  sockaddr_un address{};
  int size = sizeof(address);
  if (getsockname(socket_, reinterpret_cast<sockaddr *>(&address), &size) != 0)
    return std::unexpected(last_error());
  return detail::local_socket_name(address, static_cast<std::size_t>(size));
}

Result<std::string> LocalStream::peer_address() const
{
  detail::IoAccess::check_thread(*ctx_);
  sockaddr_un address{};
  int size = sizeof(address);
  if (getpeername(socket_, reinterpret_cast<sockaddr *>(&address), &size) != 0)
    return std::unexpected(last_error());
  return detail::local_socket_name(address, static_cast<std::size_t>(size));
}

Result<LocalPeer> LocalStream::peer_credentials() const
{
  detail::IoAccess::check_thread(*ctx_);
  if (socket_ == INVALID_SOCKET)
    return std::unexpected(win_error(WSAENOTSOCK));
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
}

LocalListener::LocalListener(LocalListener &&other) noexcept : ctx_(other.ctx_), socket_(INVALID_SOCKET)
{
  detail::require(!other.accepting_);
  socket_ = std::exchange(other.socket_, INVALID_SOCKET);
  address_ = std::move(other.address_);
  skip_success_ = other.skip_success_;
}

LocalListener::~LocalListener()
{
  detail::require(static_cast<bool>(close()));
}

Result<void> LocalListener::close()
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

Result<void> LocalListener::cancel()
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

const std::string &LocalListener::local_address() const noexcept
{
  detail::IoAccess::check_thread(*ctx_);
  return address_;
}

Task<LocalStream> LocalListener::accept()
{
  detail::IoAccess::check_execution(*ctx_);
  if (accepting_)
    co_await fail(busy());
  if (socket_ == INVALID_SOCKET)
    co_await fail(win_error(WSAENOTSOCK));
  LocalOperationFlag guard(accepting_);
  auto function = extension<LPFN_ACCEPTEX>(socket_, WSAID_ACCEPTEX);
  if (!function)
    co_await fail(function.error());

  bool skip_success = false;
  auto socket = make_socket(*ctx_, skip_success);
  if (!socket)
    co_await fail(socket.error());
  LocalStream stream(*ctx_, *socket, skip_success);

  using AcceptAwaiter = detail::SocketIoAwaiter<sizeof(sockaddr_un) + 16>;
  AcceptAwaiter operation(AcceptAwaiter::accept, *ctx_, socket_, skip_success_);
  operation.accepted = *socket;
  operation.accept_fn = *function;
  operation.address_bytes = sizeof(sockaddr_un) + 16;
  auto accepted = co_await operation;
  if (!accepted)
    co_await fail(accepted.error());

  if (setsockopt(
        *socket,
        SOL_SOCKET,
        SO_UPDATE_ACCEPT_CONTEXT,
        reinterpret_cast<const char *>(&socket_),
        sizeof(socket_)) != 0)
    co_await fail(last_error());
  co_return std::move(stream);
}

} // namespace weave
