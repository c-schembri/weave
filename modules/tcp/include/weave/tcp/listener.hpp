#pragma once

#include <weave/tcp/stream.hpp>

namespace weave {

/// Native listen queue hint and IPv6 policy; backlog does not limit live clients.
struct ListenOptions {
  int backlog = 0x7fffffff;
  // IPv6 listeners are v6-only unless dual-stack is explicitly requested.
  bool ipv6_only = true;
};

namespace tcp {

/// Bind/listen synchronously on context; port zero selects an ephemeral port.
/// The Context must outlive the listener and accepted streams. No running task is required.
Result<TcpListener> listen(Context &context, Endpoint endpoint, ListenOptions options = {});
/// Bind/listen lazily on the active Context; execution without one is a contract violation.
Task<TcpListener> listen(Endpoint endpoint, ListenOptions options = {});

/// Synchronously bind a numeric IP literal, borrowed only during this call. No DNS lookup.
/// Windows maps backlogs above 200 to a native hint capped at 65535; the default uses OS policy.
Result<TcpListener> listen(Context &context, const char *address, u16 port, int backlog = 0x7fffffff);
/// Bind a borrowed numeric IP literal on the active Context; keep address alive through setup.
Task<TcpListener> listen(const char *address, u16 port, int backlog = 0x7fffffff);

} // namespace tcp

/// Settings applied to each accepted stream before it is exposed to the caller.
struct AcceptOptions {
  bool no_delay = false;
};

/// Move-only listener bound to its original Context, with at most one pending accept.
/// The listener and Context must survive operation completion, including cancellation.
class TcpListener {
public:
  /// Transfer an idle listener; moving with a pending accept is a contract violation.
  TcpListener(TcpListener &&other) noexcept;
  TcpListener(const TcpListener &) = delete;
  /// Close the listener after pending accepts have drained.
  ~TcpListener();
  /// Accept on a compatible executing Context and configure the stream before returning it.
  /// A concurrent accept is busy; option setup errors fail the task and close the new socket.
  Task<TcpStream> accept(AcceptOptions options = {});

  /// Cached bound port, retained after close. A moved-from listener reports zero; no syscall.
  u16 local_port() const noexcept
  {
    return local_.port;
  }

  /// Copy the cached bound endpoint, including the OS-selected port; no syscall.
  Endpoint local_endpoint() const noexcept
  {
    return local_;
  }

  /// Request pending accept cancellation without joining it; its task must still drain.
  Result<void> cancel();
  /// Close synchronously when no accept is pending; otherwise return busy.
  Result<void> close();

private:
  friend Result<TcpListener> tcp::listen(Context &, Endpoint, ListenOptions);

  TcpListener(Context &ctx, std::uintptr_t socket, bool skip_success) noexcept
      : ctx_(&ctx), socket_(socket), skip_success_(skip_success)
  {
  }

  Context *ctx_;
  std::uintptr_t socket_;
  Endpoint local_{};
  bool accepting_ = false;
  bool skip_success_ = false;
};

} // namespace weave
