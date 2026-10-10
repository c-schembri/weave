#pragma once

#include "uring.hpp"
#include "address.hpp"
#include <cerrno>

namespace weave::detail {

// Native stream completion/cancellation machinery shared by TCP and local sockets.
struct SocketIoAwaiter {
  static Error native_error(int code)
  {
    return {code, std::generic_category()};
  }

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

  SocketIoAwaiter(Kind kind, Context &context, int socket) : kind(kind), context(context)
  {
    operation.context = &context;
    operation.provider = this;
    operation.socket = socket;
    if (kind == receive)
      operation.kind = detail::Operation::Kind::receive;
    if (kind == send)
      operation.kind = detail::Operation::Kind::send;
    operation.prepare = [](io_uring_sqe *entry, detail::Operation &operation) noexcept {
      auto &awaiter = *static_cast<SocketIoAwaiter *>(operation.provider);
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

} // namespace weave::detail
