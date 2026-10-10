#pragma once

#include "iocp.hpp"
#include "address.hpp"
#include <weave/io/detail/trace.hpp>
#include <mswsock.h>
#include <array>
#include <shared_mutex>

namespace weave::detail {

// Native stream completion/cancellation machinery shared by TCP and local sockets.
template <std::size_t AddressCapacity = sizeof(sockaddr_in6) + 16>
struct SocketIoAwaiter {
  static Error win_error(int code)
  {
    return {code, std::system_category()};
  }

  static Error last_error()
  {
    return win_error(WSAGetLastError());
  }

  enum Kind {
    receive,
    send,
    accept,
    connect
  } kind;

  Context &ctx;
  bool skip_success;
  detail::Operation operation{};
  SOCKET socket;
  WSABUF buffer{};
  SOCKET accepted = INVALID_SOCKET;
  const detail::SocketAddress *endpoint = nullptr;
  LPFN_ACCEPTEX accept_fn = nullptr;
  LPFN_CONNECTEX connect_fn = nullptr;
  std::array<std::byte, 2 * AddressCapacity> addresses{};
  DWORD address_bytes = sizeof(sockaddr_in) + 16;
  CancelToken cancellation;

  struct CancelOperation {
    HANDLE socket;
    OVERLAPPED *operation;

    void operator()() const noexcept
    {
      if (!CancelIoEx(socket, operation))
        detail::require(GetLastError() == ERROR_NOT_FOUND);
    }
  };

  std::optional<std::stop_callback<CancelOperation>> cancellation_callback;

  SocketIoAwaiter(Kind k, Context &c, SOCKET s, bool skip) : kind(k), ctx(c), skip_success(skip), socket(s)
  {
  }

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> continuation) noexcept
  {
    detail::IoAccess::check_execution(ctx);
    auto &io = detail::IoAccess::state(ctx);
    cancellation = continuation.promise().cancellation;
    if (cancellation.stop_requested()) {
      operation.error = std::make_error_code(std::errc::operation_canceled);
      return false;
    }

    // Exclude shutdown cancellation until the operation has been submitted.
    std::shared_lock submission(io.io_mutex_, std::defer_lock);

    if (io.scheduler_group_)
      submission.lock();

    if (io.stopping_.load(std::memory_order_relaxed)) {
      operation.error = win_error(ERROR_OPERATION_ABORTED);
      return false;
    }

    operation.event.state = continuation.address();
    operation.event.invoke = [](void *state) noexcept {
      std::coroutine_handle<>::from_address(state).resume();
    };
    operation.event.executor = detail::current_executor;
    operation.context = &ctx;

    DWORD flags = 0;
    DWORD bytes = 0;
    int result = 0;
#if defined(WEAVE_PROFILE_RUNTIME)
    const auto submission_start = std::chrono::steady_clock::now();
#endif
    detail::trace(detail::TraceEvent::io_submit_begin, continuation.address(), kind);
    switch (kind) {
    case receive:
#if defined(WEAVE_PROFILE_RUNTIME)
      detail::IoAccess::count(ctx, io.metrics_.read_calls);
#endif
      result = WSARecv(socket, &buffer, 1, &bytes, &flags, &operation.overlapped, nullptr);
      break;
    case send:
#if defined(WEAVE_PROFILE_RUNTIME)
      detail::IoAccess::count(ctx, io.metrics_.write_calls);
#endif
      result = WSASend(socket, &buffer, 1, &bytes, 0, &operation.overlapped, nullptr);
      break;
    case accept: {
      auto succeeded = accept_fn(
        socket,
        accepted,
        addresses.data(),
        0,
        address_bytes,
        address_bytes,
        &bytes,
        &operation.overlapped);
      result = succeeded ? 0 : SOCKET_ERROR;
      break;
    }
    case connect: {
      auto succeeded = connect_fn(socket, endpoint->data(), endpoint->size, nullptr, 0, &bytes, &operation.overlapped);
      result = succeeded ? 0 : SOCKET_ERROR;
      break;
    }
    }

    const auto error = result != 0 ? WSAGetLastError() : 0;
    detail::trace(detail::TraceEvent::io_submit_end, continuation.address(), error);
#if defined(WEAVE_PROFILE_RUNTIME)
    const auto submission_time = std::chrono::steady_clock::now() - submission_start;
    const auto submission_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(submission_time).count();
    detail::IoAccess::count(ctx, io.metrics_.submission_ns, submission_ns);
    if (kind == receive)
      detail::IoAccess::count(ctx, io.metrics_.read_submission_ns, submission_ns);
    if (kind == send)
      detail::IoAccess::count(ctx, io.metrics_.write_submission_ns, submission_ns);
#endif
    if (result != 0) {
      if (error != WSA_IO_PENDING) {
        operation.error = win_error(error);
        return false;
      }
    }
#if defined(WEAVE_PROFILE_RUNTIME)
    if (result == 0)
      detail::IoAccess::count(ctx, io.metrics_.immediate_successes);
#endif
    if (result == 0 && skip_success) {
      operation.transferred = bytes;

      // Limit inline chaining so a busy stream cannot indefinitely starve peers.
      auto &budget = io.scheduler_group_ ? detail::inline_budget : io.inline_budget_;
      if (++budget < 32) {
        detail::IoAccess::count(ctx, io.metrics_.inline_completions);
        return false;
      }

      budget = 0;
      detail::IoAccess::count(ctx, io.metrics_.fairness_posts);
      auto posted = PostQueuedCompletionStatus(io.port_, bytes, 0, &operation.overlapped);
      detail::require(posted != FALSE);
    }

    // Without skip-success, synchronous success still queues an OS packet.
    detail::IoAccess::count(ctx, io.metrics_.submitted);
    if (!(result == 0 && skip_success)) {
      cancellation_callback.emplace(
        cancellation.native_token(),
        CancelOperation{reinterpret_cast<HANDLE>(socket), &operation.overlapped});
    }
    return true;
  }

  Result<std::size_t> await_resume() noexcept
  {
    // Wait for a racing CancelIoEx callback before releasing the native record/socket.
    cancellation_callback.reset();
    // Keep the socket alive through error translation, before the awaiter's guard is released.
    if (operation.failed) {
      DWORD flags = 0, transferred = 0;
      if (!WSAGetOverlappedResult(socket, &operation.overlapped, &transferred, FALSE, &flags))
        operation.error = last_error();
    }

    if (operation.error.value() == ERROR_OPERATION_ABORTED && (cancellation.stop_requested() || ctx.stop_requested()))
      operation.error = std::make_error_code(std::errc::operation_canceled);
    if (operation.error)
      return std::unexpected(operation.error);
#if defined(WEAVE_PROFILE_RUNTIME)
    auto &io = detail::IoAccess::state(ctx);
    if (kind == receive)
      detail::IoAccess::count(ctx, io.metrics_.read_bytes, operation.transferred);
    if (kind == send)
      detail::IoAccess::count(ctx, io.metrics_.write_bytes, operation.transferred);
#endif
    return operation.transferred;
  }
};

} // namespace weave::detail
