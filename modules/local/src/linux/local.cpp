#include <weave/local.hpp>
#include "linux/socket.hpp"
#include "../address.hpp"
#include <algorithm>
#include <unistd.h>

namespace weave {

namespace {

constexpr std::uintptr_t invalid_socket = std::numeric_limits<std::uintptr_t>::max();

Error native_error(int code)
{
  return {code, std::generic_category()};
}

Error last_error()
{
  return native_error(errno);
}

Error busy()
{
  return std::make_error_code(std::errc::operation_in_progress);
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

using LocalIoAwaiter = detail::SocketIoAwaiter;

Result<int> make_socket(Context &context)
{
  detail::IoAccess::check_thread(context);
  if (context.stop_requested())
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));

  const auto socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (socket < 0)
    return std::unexpected(last_error());
  auto attached = detail::IoAccess::attach(context, socket, false);
  if (!attached) {
    ::close(socket);
    return std::unexpected(attached.error());
  }
  return socket;
}

Result<void> close_socket(Context &context, std::uintptr_t socket)
{
  return detail::IoAccess::close(context, socket, [](std::uintptr_t handle) noexcept -> Error {
    // Linux releases the descriptor even if close reports EINTR; never retry it.
    if (::close(static_cast<int>(handle)) != 0 && errno != EINTR)
      return last_error();
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

  bool skip_success = false;
  auto socket = make_socket(context);
  if (!socket)
    co_await fail(socket.error());
  LocalStream stream(context, *socket, skip_success);

  LocalIoAwaiter operation(LocalIoAwaiter::connect, context, *socket);
  operation.endpoint = &*native;
  auto connected = co_await operation;
  if (!connected)
    co_await fail(connected.error());

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

  bool skip_success = false;
  auto socket = make_socket(context);
  if (!socket)
    return std::unexpected(socket.error());
  LocalListener listener(context, *socket, skip_success, std::move(address));
  if (bind(*socket, native->data(), native->size) != 0)
    return std::unexpected(last_error());

  auto native_backlog = backlog;
  if (::listen(*socket, native_backlog) != 0)
    return std::unexpected(last_error());
  return listener;
}

LocalStream::LocalStream(LocalStream &&other) noexcept : ctx_(other.ctx_), socket_(invalid_socket)
{
  detail::require(!other.reading_ && !other.writing_);
  socket_ = std::exchange(other.socket_, invalid_socket);
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
  if (socket_ != invalid_socket) {
    auto result = close_socket(*ctx_, socket_);
    if (!result)
      return result;
    socket_ = invalid_socket;
  }
  return {};
}

Result<void> LocalStream::shutdown_send()
{
  detail::IoAccess::check_thread(*ctx_);
  if (writing_)
    return std::unexpected(busy());
  if (shutdown(static_cast<int>(socket_), SHUT_WR) != 0)
    return std::unexpected(last_error());
  return {};
}

Result<void> LocalStream::cancel()
{
  detail::IoAccess::check_thread(*ctx_);
  if (socket_ == invalid_socket)
    return std::unexpected(native_error(EBADF));
  detail::IoAccess::cancel_socket(*ctx_, static_cast<int>(socket_));
  return {};
}

Task<std::size_t> LocalStream::read(std::span<std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (reading_)
    co_await fail(busy());
  if (socket_ == invalid_socket)
    co_await fail(native_error(EBADF));
  if (buffer.empty())
    co_return std::size_t{0};

  LocalOperationFlag guard(reading_);
  LocalIoAwaiter operation(LocalIoAwaiter::receive, *ctx_, static_cast<int>(socket_));
  operation.buffer = buffer.data();
  operation.size = static_cast<unsigned>(std::min(buffer.size(), std::size_t{65536}));
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
  if (socket_ == invalid_socket)
    co_await fail(native_error(EBADF));

  LocalOperationFlag guard(reading_);
  while (!buffer.empty()) {
    LocalIoAwaiter operation(LocalIoAwaiter::receive, *ctx_, static_cast<int>(socket_));
    operation.buffer = buffer.data();
    operation.size = static_cast<unsigned>(std::min(buffer.size(), std::size_t{65536}));
    auto result = co_await operation;
    if (!result)
      co_await fail(result.error());
    if (*result == 0)
      co_await fail(std::errc::connection_reset);
    buffer = buffer.subspan(*result);
  }
}

Task<void> LocalStream::write_all(std::span<const std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (writing_)
    co_await fail(busy());
  if (socket_ == invalid_socket)
    co_await fail(native_error(EBADF));

  LocalOperationFlag guard(writing_);
  while (!buffer.empty()) {
    LocalIoAwaiter operation(LocalIoAwaiter::send, *ctx_, static_cast<int>(socket_));
    operation.buffer = const_cast<std::byte *>(buffer.data());
    operation.size = static_cast<unsigned>(std::min(buffer.size(), std::size_t{65536}));
    auto result = co_await operation;
    if (!result)
      co_await fail(result.error());
    if (*result == 0)
      co_await fail(std::errc::broken_pipe);
    buffer = buffer.subspan(*result);
  }
}

Result<std::string> LocalStream::local_address() const
{
  detail::IoAccess::check_thread(*ctx_);
  sockaddr_un address{};
  socklen_t size = sizeof(address);
  if (getsockname(static_cast<int>(socket_), reinterpret_cast<sockaddr *>(&address), &size) != 0)
    return std::unexpected(last_error());
  return detail::local_socket_name(address, static_cast<std::size_t>(size));
}

Result<std::string> LocalStream::peer_address() const
{
  detail::IoAccess::check_thread(*ctx_);
  sockaddr_un address{};
  socklen_t size = sizeof(address);
  if (getpeername(static_cast<int>(socket_), reinterpret_cast<sockaddr *>(&address), &size) != 0)
    return std::unexpected(last_error());
  return detail::local_socket_name(address, static_cast<std::size_t>(size));
}

Result<LocalPeer> LocalStream::peer_credentials() const
{
  detail::IoAccess::check_thread(*ctx_);
  ucred credentials{};
  socklen_t size = sizeof(credentials);
  if (getsockopt(static_cast<int>(socket_), SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0)
    return std::unexpected(last_error());
  if (size != sizeof(credentials) || credentials.pid < 0)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  return LocalPeer{static_cast<u64>(credentials.pid), credentials.uid, credentials.gid};
}

LocalListener::LocalListener(LocalListener &&other) noexcept : ctx_(other.ctx_), socket_(invalid_socket)
{
  detail::require(!other.accepting_);
  socket_ = std::exchange(other.socket_, invalid_socket);
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
  if (socket_ != invalid_socket) {
    auto result = close_socket(*ctx_, socket_);
    if (!result)
      return result;
    socket_ = invalid_socket;
  }
  return {};
}

Result<void> LocalListener::cancel()
{
  detail::IoAccess::check_thread(*ctx_);
  if (socket_ == invalid_socket)
    return std::unexpected(native_error(EBADF));
  detail::IoAccess::cancel_socket(*ctx_, static_cast<int>(socket_));
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
  if (socket_ == invalid_socket)
    co_await fail(native_error(EBADF));
  LocalOperationFlag guard(accepting_);
  LocalIoAwaiter operation(LocalIoAwaiter::accept, *ctx_, static_cast<int>(socket_));
  auto accepted = co_await operation;
  if (!accepted)
    co_await fail(accepted.error());
  auto socket = static_cast<int>(*accepted);
  auto attached = detail::IoAccess::attach(*ctx_, socket, false);
  if (!attached) {
    auto error = attached.error();
    ::close(socket);
    co_await fail(error);
  }
  co_return LocalStream(*ctx_, socket, false);
}

} // namespace weave
