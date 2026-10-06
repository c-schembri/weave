#pragma once

#include <weave/tcp/stream.hpp>

namespace weave {

struct ListenOptions {
  int backlog = 0x7fffffff;
  // IPv6 listeners are v6-only unless dual-stack is explicitly requested.
  bool ipv6_only = true;
};

namespace tcp {

Result<TcpListener> listen(Context &context, Endpoint endpoint, ListenOptions options = {});
Task<TcpListener> listen(Endpoint endpoint, ListenOptions options = {});

// Windows translates explicit backlogs above 200 to a native hint, capped at 65535.
// The default leaves the queue size to the OS; backlog is not a limit on live clients.
Result<TcpListener> listen(Context &context, const char *address, u16 port, int backlog = 0x7fffffff);
// Numeric IPv4/IPv6 only. Borrowed address must survive lazy setup.
Task<TcpListener> listen(const char *address, u16 port, int backlog = 0x7fffffff);

} // namespace tcp

struct AcceptOptions {
  bool no_delay = false;
};

class TcpListener {
public:
  TcpListener(TcpListener &&other) noexcept;
  TcpListener(const TcpListener &) = delete;
  ~TcpListener();
  // Configure the accepted stream before returning it; setup errors fail the Task.
  Task<TcpStream> accept(AcceptOptions options = {});

  // Cached bound port, retained after close. A moved-from listener reports zero.
  u16 local_port() const noexcept
  {
    return local_.port;
  }

  Endpoint local_endpoint() const noexcept
  {
    return local_;
  }

  Result<void> cancel();
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
