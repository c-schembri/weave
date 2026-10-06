#pragma once

#include <weave/io.hpp>
#include <span>

namespace weave {

class TcpStream;
class TcpListener;

namespace tcp {

Task<TcpStream> connect(Context &context, const char *ipv4, u16 port);
// Lazy setup on the executing Context. ipv4 must remain alive until setup finishes.
Task<TcpStream> connect(const char *ipv4, u16 port);

} // namespace tcp

class TcpStream {
public:
  TcpStream(TcpStream &&other) noexcept;
  TcpStream(const TcpStream &) = delete;
  ~TcpStream();
  Task<std::size_t> read(std::span<std::byte> buffer);
  Task<void> read_exactly(std::span<std::byte> buffer);
  Task<void> write_all(std::span<const std::byte> buffer);
  Result<void> shutdown_send();
  Result<void> cancel();
  Result<void> close();
  Result<void> no_delay(bool enabled = true);

private:
  friend Task<TcpStream> tcp::connect(Context &, const char *, u16);
  friend class TcpListener;

  TcpStream(Context &ctx, std::uintptr_t socket, bool skip_success) noexcept
      : ctx_(&ctx), socket_(socket), skip_success_(skip_success)
  {
  }

  Context *ctx_;
  std::uintptr_t socket_;
  bool reading_ = false;
  bool writing_ = false;
  bool skip_success_ = false;
};

} // namespace weave
