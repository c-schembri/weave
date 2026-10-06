#include <weave/tcp.hpp>
#include "linux/uring.hpp"
#include "linux/address.hpp"
#include <weave/resolve.hpp>
#include <algorithm>
#include <cerrno>
#include <netinet/tcp.h>
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

Error invalid()
{
  return std::make_error_code(std::errc::invalid_argument);
}

Error busy()
{
  return std::make_error_code(std::errc::operation_in_progress);
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

struct TcpIoAwaiter {
  enum Kind {
    receive,
    send,
    accept,
    connect
  } kind;

  Context &context;
  detail::Operation operation{};
  void *buffer = nullptr;
  unsigned size = 0;
  const detail::SocketAddress *endpoint = nullptr;

  struct Cancel {
    detail::Operation *operation;

    void operator()() const noexcept
    {
      detail::IoAccess::cancel(*operation);
    }
  };

  std::optional<std::stop_callback<Cancel>> cancellation;
  std::optional<std::stop_callback<Cancel>> shutdown;

  TcpIoAwaiter(Kind kind, Context &context, int socket) : kind(kind), context(context)
  {
    operation.context = &context;
    operation.provider = this;
    operation.socket = socket;
    if (kind == receive)
      operation.kind = detail::Operation::Kind::receive;
    if (kind == send)
      operation.kind = detail::Operation::Kind::send;
    operation.prepare = [](io_uring_sqe *entry, detail::Operation &operation) noexcept {
      auto &awaiter = *static_cast<TcpIoAwaiter *>(operation.provider);
      switch (awaiter.kind) {
      case receive:
        io_uring_prep_recv(entry, operation.socket, awaiter.buffer, awaiter.size, 0);
        break;
      case send:
        io_uring_prep_send(entry, operation.socket, awaiter.buffer, awaiter.size, MSG_NOSIGNAL);
        break;
      case accept:
        io_uring_prep_accept(entry, operation.socket, nullptr, nullptr, SOCK_CLOEXEC);
        break;
      case connect:
        io_uring_prep_connect(entry, operation.socket, awaiter.endpoint->data(), awaiter.endpoint->size);
        break;
      }
    };
  }

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> continuation) noexcept
  {
    detail::IoAccess::check_execution(context);
    const auto token = continuation.promise().cancellation;
    const auto stopping = detail::context_cancellation(context);
    if (token.stop_requested() || stopping.stop_requested()) {
      operation.result = -ECANCELED;
      return false;
    }

    operation.event.state = continuation.address();
    operation.event.invoke = [](void *state) noexcept {
      std::coroutine_handle<>::from_address(state).resume();
    };
    operation.event.executor = detail::current_executor;
    cancellation.emplace(token.native_token(), Cancel{&operation});
    shutdown.emplace(stopping.native_token(), Cancel{&operation});
    detail::IoAccess::submit(operation);
    return true;
  }

  Result<std::size_t> await_resume() noexcept
  {
    cancellation.reset();
    shutdown.reset();
    if (operation.result == -EINTR && (operation.cancelled || context.stop_requested()))
      return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    if (operation.result < 0)
      return std::unexpected(native_error(-operation.result));
#if defined(WEAVE_PROFILE_RUNTIME)
    auto &state = detail::IoAccess::state(context);
    if (kind == receive)
      detail::IoAccess::count(context, state.metrics_.read_bytes, operation.result);
    if (kind == send)
      detail::IoAccess::count(context, state.metrics_.write_bytes, operation.result);
#endif
    return static_cast<std::size_t>(operation.result);
  }
};

Result<int> make_socket(Context &context, int family)
{
  detail::IoAccess::check_thread(context);
  if (context.stop_requested())
    return std::unexpected(std::make_error_code(std::errc::operation_canceled));

  const auto socket = ::socket(family, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
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

Task<TcpListener> tcp::listen(const char *address, u16 port, int backlog)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  auto listener = tcp::listen(*context, address, port, backlog);
  if (!listener)
    co_await fail(listener.error());
  co_return std::move(*listener);
}

Task<TcpListener> tcp::listen(Endpoint endpoint, ListenOptions options)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  auto listener = tcp::listen(*context, endpoint, options);
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
  auto socket = make_socket(context, native.data()->sa_family);
  if (!socket)
    return std::unexpected(socket.error());

  TcpListener listener(context, *socket, false);
  const int reuse = 1;
  if (setsockopt(*socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
    return std::unexpected(last_error());

  if (endpoint.address.is_v6()) {
    const int only = options.ipv6_only;
    if (setsockopt(*socket, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(only)) != 0)
      return std::unexpected(last_error());
  }
  if (bind(*socket, native.data(), native.size) != 0 || ::listen(*socket, options.backlog) != 0)
    return std::unexpected(last_error());

  sockaddr_storage local{};
  socklen_t size = sizeof(local);
  if (getsockname(*socket, reinterpret_cast<sockaddr *>(&local), &size) != 0)
    return std::unexpected(last_error());
  auto bound = detail::socket_endpoint(reinterpret_cast<sockaddr *>(&local), size);
  if (!bound)
    return std::unexpected(bound.error());
  listener.local_ = *bound;
  return listener;
}

Task<TcpStream> tcp::connect(const char *host, u16 port)
{
  auto *context = detail::current_context;
  detail::require(context != nullptr);
  co_return co_await tcp::connect(*context, host, port);
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
  auto native = detail::socket_address(endpoint);
  auto socket = make_socket(context, native.data()->sa_family);
  if (!socket)
    co_await fail(socket.error());

  TcpStream stream(context, *socket, false);
  TcpIoAwaiter operation(TcpIoAwaiter::connect, context, *socket);
  operation.endpoint = &native;
  auto connected = co_await operation;
  if (!connected)
    co_await fail(connected.error());
  co_return std::move(stream);
}

TcpStream::TcpStream(TcpStream &&other) noexcept : ctx_(other.ctx_), socket_(invalid_socket)
{
  detail::require(!other.reading_ && !other.writing_);
  socket_ = std::exchange(other.socket_, invalid_socket);
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
  if (socket_ != invalid_socket) {
    auto result = close_socket(*ctx_, socket_);
    if (!result)
      return result;
    socket_ = invalid_socket;
  }
  return {};
}

Result<void> TcpStream::no_delay(bool enabled)
{
  detail::IoAccess::check_thread(*ctx_);
  const int value = enabled;
  if (setsockopt(static_cast<int>(socket_), IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0)
    return std::unexpected(last_error());
  return {};
}

Result<void> TcpStream::shutdown_send()
{
  detail::IoAccess::check_thread(*ctx_);
  if (writing_)
    return std::unexpected(busy());
  if (shutdown(static_cast<int>(socket_), SHUT_WR) != 0)
    return std::unexpected(last_error());
  return {};
}

Result<Endpoint> TcpStream::local_endpoint() const
{
  detail::IoAccess::check_thread(*ctx_);
  sockaddr_storage address{};
  socklen_t size = sizeof(address);
  if (getsockname(static_cast<int>(socket_), reinterpret_cast<sockaddr *>(&address), &size) != 0)
    return std::unexpected(last_error());
  return detail::socket_endpoint(reinterpret_cast<sockaddr *>(&address), size);
}

Result<Endpoint> TcpStream::peer_endpoint() const
{
  detail::IoAccess::check_thread(*ctx_);
  sockaddr_storage address{};
  socklen_t size = sizeof(address);
  if (getpeername(static_cast<int>(socket_), reinterpret_cast<sockaddr *>(&address), &size) != 0)
    return std::unexpected(last_error());
  return detail::socket_endpoint(reinterpret_cast<sockaddr *>(&address), size);
}

Result<void> TcpStream::cancel()
{
  detail::IoAccess::check_thread(*ctx_);
  if (socket_ == invalid_socket)
    return std::unexpected(native_error(EBADF));
  detail::IoAccess::cancel_socket(*ctx_, static_cast<int>(socket_));
  return {};
}

Task<std::size_t> TcpStream::read(std::span<std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (reading_)
    co_await fail(busy());
  if (socket_ == invalid_socket)
    co_await fail(native_error(EBADF));
  if (buffer.empty())
    co_return std::size_t{0};

  TcpOperationFlag guard(reading_);
  TcpIoAwaiter operation(TcpIoAwaiter::receive, *ctx_, static_cast<int>(socket_));
  operation.buffer = buffer.data();
  operation.size = static_cast<unsigned>(std::min(buffer.size(), std::size_t{65536}));
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
  if (socket_ == invalid_socket)
    co_await fail(native_error(EBADF));

  TcpOperationFlag guard(reading_);
  while (!buffer.empty()) {
    TcpIoAwaiter operation(TcpIoAwaiter::receive, *ctx_, static_cast<int>(socket_));
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

Task<void> TcpStream::write_all(std::span<const std::byte> buffer)
{
  detail::IoAccess::check_execution(*ctx_);
  if (writing_)
    co_await fail(busy());
  if (socket_ == invalid_socket)
    co_await fail(native_error(EBADF));

  TcpOperationFlag guard(writing_);
  while (!buffer.empty()) {
    TcpIoAwaiter operation(TcpIoAwaiter::send, *ctx_, static_cast<int>(socket_));
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

TcpListener::TcpListener(TcpListener &&other) noexcept : ctx_(other.ctx_), socket_(invalid_socket)
{
  detail::require(!other.accepting_);
  socket_ = std::exchange(other.socket_, invalid_socket);
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
  if (socket_ != invalid_socket) {
    auto result = close_socket(*ctx_, socket_);
    if (!result)
      return result;
    socket_ = invalid_socket;
  }
  return {};
}

Result<void> TcpListener::cancel()
{
  detail::IoAccess::check_thread(*ctx_);
  if (socket_ == invalid_socket)
    return std::unexpected(native_error(EBADF));
  detail::IoAccess::cancel_socket(*ctx_, static_cast<int>(socket_));
  return {};
}

Task<TcpStream> TcpListener::accept(AcceptOptions options)
{
  detail::IoAccess::check_execution(*ctx_);
  if (accepting_)
    co_await fail(busy());
  if (socket_ == invalid_socket)
    co_await fail(native_error(EBADF));

  TcpOperationFlag guard(accepting_);
  TcpIoAwaiter operation(TcpIoAwaiter::accept, *ctx_, static_cast<int>(socket_));
  auto accepted = co_await operation;
  if (!accepted)
    co_await fail(accepted.error());

  const auto socket = static_cast<int>(*accepted);
  auto attached = detail::IoAccess::attach(*ctx_, socket, false);
  if (!attached) {
    ::close(socket);
    co_await fail(attached.error());
  }
  TcpStream stream(*ctx_, socket, false);
  if (options.no_delay) {
    auto configured = stream.no_delay();
    if (!configured)
      co_await fail(configured.error());
  }
  co_return std::move(stream);
}

} // namespace weave
