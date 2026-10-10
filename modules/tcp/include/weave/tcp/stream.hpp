#pragma once

#include <weave/io.hpp>
#include <weave/address.hpp>
#include <chrono>
#include <span>
#include <string>
#include <vector>

namespace weave {

class TcpStream;
class TcpListener;

struct TcpKeepAliveOptions {
  bool enabled = true;
  std::chrono::seconds idle{0};
  std::chrono::seconds interval{0};
  u32 probes = 0;
};

namespace tcp {

Task<TcpStream> connect(Context &context, Endpoint endpoint);
Task<TcpStream> connect(Endpoint endpoint);
// Try endpoints in order. Cancellation stops the attempt sequence.
Task<TcpStream> connect(Context &context, std::vector<Endpoint> endpoints);
Task<TcpStream> connect(std::vector<Endpoint> endpoints);
// Numeric addresses bypass DNS; names are resolved asynchronously.
// Borrowed host strings must remain alive until setup finishes.
Task<TcpStream> connect(Context &context, const char *host, u16 port);
Task<TcpStream> connect(const char *host, u16 port);
Task<TcpStream> connect(Context &context, std::string host, u16 port);
Task<TcpStream> connect(std::string host, u16 port);

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
  // Zero tuning values leave the current native settings unchanged.
  // Native changes are not atomic: earlier changes can survive a later failure.
  Result<void> keep_alive(TcpKeepAliveOptions options = {});
  Result<TcpKeepAliveOptions> keep_alive_options() const;
  // TCP_USER_TIMEOUT is supported on Linux, not Windows.
  Result<void> user_timeout(std::chrono::milliseconds timeout);
  Result<std::chrono::milliseconds> user_timeout() const;
  Result<Endpoint> local_endpoint() const;
  Result<Endpoint> peer_endpoint() const;

private:
  friend Task<TcpStream> tcp::connect(Context &, Endpoint);
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
