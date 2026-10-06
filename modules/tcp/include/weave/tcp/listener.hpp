#pragma once

#include <weave/tcp/stream.hpp>

namespace weave {

namespace tcp {

Result<TcpListener> listen(Context &context, const char *ipv4, u16 port, int backlog = 0x7fffffff);
// Lazy setup on the executing Context. ipv4 must remain alive until setup finishes.
Task<TcpListener> listen(const char *ipv4, u16 port, int backlog = 0x7fffffff);

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
    return port_;
  }

  Result<void> cancel();
  Result<void> close();

private:
  friend Result<TcpListener> tcp::listen(Context &, const char *, u16, int);

  TcpListener(Context &ctx, std::uintptr_t socket, bool skip_success) noexcept
      : ctx_(&ctx), socket_(socket), skip_success_(skip_success)
  {
  }

  Context *ctx_;
  std::uintptr_t socket_;
  u16 port_ = 0;
  bool accepting_ = false;
  bool skip_success_ = false;
};

} // namespace weave
