#pragma once

#include <weave/tcp/stream.hpp>

namespace weave {

namespace tcp {

Result<TcpListener> listen(Context &context, const char *ipv4, u16 port, int backlog = 0x7fffffff);

} // namespace tcp

class TcpListener {
public:
  TcpListener(TcpListener &&other) noexcept;
  TcpListener(const TcpListener &) = delete;
  ~TcpListener();
  Task<TcpStream> accept();
  Result<u16> local_port() const;
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
  bool accepting_ = false;
  bool skip_success_ = false;
};

} // namespace weave
