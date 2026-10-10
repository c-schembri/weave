#pragma once

#include "gss_pool.hpp"
#include <weave/tcp.hpp>
#include <variant>

namespace weave::pg::detail {

class GssStream {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit GssStream(std::unique_ptr<Impl> impl) noexcept;
  Impl &state() const noexcept;

public:
  GssStream(GssStream &&) noexcept;
  GssStream &operator=(GssStream &&) noexcept;
  GssStream(const GssStream &) = delete;
  ~GssStream();

  // The GSSENCRequest response has already been read, exactly one byte at a time.
  static Task<GssStream> establish(
    TcpStream socket,
    GssContext provider,
    std::string host,
    GssOptions options,
    Diagnostic &diagnostic);

  Task<std::size_t> read(std::span<std::byte> buffer);
  Task<void> write_all(std::span<const std::byte> buffer);
  Task<void> finish();
  Result<void> shutdown_send() noexcept;
  Result<void> close() noexcept;
  Result<void> cancel() noexcept;
};

using GssTransport = std::variant<TcpStream, GssStream>;

// Only an explicit N may return plaintext; all negotiation/provider failures are terminal.
Task<GssTransport> gss_client(
  TcpStream socket,
  GssContext provider,
  std::string host,
  GssOptions options,
  GssEncryption mode,
  Diagnostic &diagnostic,
  bool require_channel_binding = false);

} // namespace weave::pg::detail
