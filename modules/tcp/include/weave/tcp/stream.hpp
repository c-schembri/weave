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

/// Native keepalive policy. Zero tuning values preserve existing OS settings.
struct TcpKeepAliveOptions {
  bool enabled = true;
  std::chrono::seconds idle{0};
  std::chrono::seconds interval{0};
  u32 probes = 0;
};

namespace tcp {

/// Connect a copied endpoint on the supplied Context; that Context must outlive the stream.
Task<TcpStream> connect(Context &context, Endpoint endpoint);
/// Connect on the active Context at execution time; no active Context is a contract violation.
Task<TcpStream> connect(Endpoint endpoint);
/// Own the endpoint list and try in order on context. Cancellation stops further attempts.
Task<TcpStream> connect(Context &context, std::vector<Endpoint> endpoints);
/// Own the endpoint list and try in order on the active Context; no parallel racing is implied.
Task<TcpStream> connect(std::vector<Endpoint> endpoints);
/// Connect a borrowed NUL-terminated host on context; keep it alive through setup/cancellation.
/// Numeric addresses bypass DNS; names are resolved asynchronously.
Task<TcpStream> connect(Context &context, const char *host, u16 port);
/// Borrow host through lazy setup and connect on the active Context when executed.
Task<TcpStream> connect(const char *host, u16 port);
/// Own host before suspension: copy lvalues, move rvalues. Resolve names and connect on context.
Task<TcpStream> connect(Context &context, std::string host, u16 port);
/// Own host and connect on the active Context; numeric literals bypass DNS.
Task<TcpStream> connect(std::string host, u16 port);

} // namespace tcp

/// Move-only connected socket retaining its original Context binding.
/// Keep the Context and stream alive through completion, including cancellation. One read and
/// one write may be pending; duplicate operations in either direction fail as busy.
/// Synchronous socket methods also require owner/compatible runtime execution, not arbitrary external threads.
class TcpStream {
public:
  /// Transfer an idle socket; moving during an active operation is a contract violation.
  TcpStream(TcpStream &&other) noexcept;
  TcpStream(const TcpStream &) = delete;
  /// Close the idle socket. Pending operations must already have drained.
  ~TcpStream();
  /// Read up to buffer.size() bytes; zero means EOF (or an empty buffer).
  /// Borrows writable storage through completion/cancellation; execute in a compatible Context.
  Task<std::size_t> read(std::span<std::byte> buffer);
  /// Fill the borrowed buffer or fail with connection_reset on premature EOF.
  Task<void> read_exactly(std::span<std::byte> buffer);
  /// Send the entire borrowed buffer, retaining it through completion/cancellation.
  /// Success is transport acceptance, not acknowledgement by the peer application.
  Task<void> write_all(std::span<const std::byte> buffer);
  /// Synchronously half-close sending; reads remain available. Native errors are returned.
  Result<void> shutdown_send();
  /// Request cancellation of pending native I/O without waiting; awaiters must still drain.
  Result<void> cancel();
  /// Close synchronously when idle; reports busy rather than destroying pending I/O storage.
  Result<void> close();
  /// Synchronously toggle TCP_NODELAY; closed sockets and native option failures return errors.
  Result<void> no_delay(bool enabled = true);
  /// Apply native keepalive settings synchronously. Zero tuning values preserve current settings.
  /// Changes are not atomic: earlier changes can survive a later native failure.
  Result<void> keep_alive(TcpKeepAliveOptions options = {});
  /// Read current native keepalive settings, or return a native query error.
  Result<TcpKeepAliveOptions> keep_alive_options() const;
  /// Set Linux TCP_USER_TIMEOUT synchronously; Windows returns operation_not_supported.
  Result<void> user_timeout(std::chrono::milliseconds timeout);
  /// Query Linux TCP_USER_TIMEOUT; Windows returns operation_not_supported.
  Result<std::chrono::milliseconds> user_timeout() const;
  /// Query the bound endpoint synchronously; returns an owning value or native error.
  Result<Endpoint> local_endpoint() const;
  /// Query the connected peer endpoint synchronously; returns an owning value or native error.
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
