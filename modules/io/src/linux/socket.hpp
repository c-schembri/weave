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
  bool *receive_nonempty = nullptr;

  struct Cancel {
    detail::Operation *operation;

    void operator()() const noexcept
    {
      detail::IoAccess::cancel(*operation);
    }
  };

  std::optional<std::stop_callback<Cancel>> cancellation;

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
#if defined(IORING_FEAT_RECVSEND_BUNDLE)
        // Bundle-capable kernels also support poll-first receives; older kernels keep the ordinary path.
        if (awaiter.size && awaiter.receive_nonempty && !*awaiter.receive_nonempty &&
          (IoAccess::state(*operation.context).ring_.features & IORING_FEAT_RECVSEND_BUNDLE))
          entry->ioprio |= IORING_RECVSEND_POLL_FIRST;
#endif
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
    return await_suspend(continuation, continuation.promise().cancellation);
  }

  bool await_suspend(std::coroutine_handle<> continuation, const CancelToken &token) noexcept
  {
    detail::IoAccess::check_execution(context);
    if (token.stop_requested() || context.stop_requested()) {
      operation.result = -ECANCELED;
      return false;
    }

    operation.event.state = continuation.address();
    operation.event.invoke = [](void *state) noexcept {
      std::coroutine_handle<>::from_address(state).resume();
    };
    operation.event.executor = detail::current_executor;
    auto &io = detail::IoAccess::state(context);
    if (kind == send && io.options_.skip_successful_completions) {
      // Synchronous attempts do not touch the ring; any execution-compatible worker can try them.
#if defined(WEAVE_PROFILE_RUNTIME)
      detail::IoAccess::count(context, io.metrics_.write_calls);
#endif
      const auto transferred = ::send(operation.socket, buffer, size, MSG_NOSIGNAL | MSG_DONTWAIT);
      const auto error = transferred < 0 ? errno : 0;
      if (transferred >= 0 || (error != EAGAIN && error != EWOULDBLOCK)) {
        operation.result = transferred < 0 ? -error : static_cast<int>(transferred);
        operation.native_done = true;
        if (transferred < 0) {
          operation.cancelled = token.stop_requested();
          return false;
        }

        detail::IoAccess::count(context, io.metrics_.immediate_successes);
        if (++detail::inline_budget < 32) {
          detail::IoAccess::count(context, io.metrics_.inline_completions);
          return false;
        }

        detail::inline_budget = 0;
        detail::IoAccess::count(context, io.metrics_.fairness_posts);
        detail::post(context, operation.event);
        return true;
      }
    }

    cancellation.emplace(token.native_token(), Cancel{&operation});
    detail::IoAccess::submit(operation);
    return true;
  }

  Result<std::size_t> await_resume() noexcept
  {
    cancellation.reset();
    if (receive_nonempty)
      *receive_nonempty = operation.result < 0 || operation.socket_nonempty;
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
