#pragma once

#include <weave/tls/context.hpp>
#include <weave/tls/stream.hpp>
#include <weave/tcp.hpp>

namespace weave::tls {

/// Own credentials, host and policy; resolve/connect TCP on the active Context, then verify TLS.
/// Copies lvalue strings and moves rvalues. The handshake timeout does not bound TCP/DNS setup.
inline Task<TlsStream<TcpStream>> connect(
  TlsContext context,
  std::string host,
  u16 port,
  TlsHandshakeOptions options = {})
{
  auto transport = co_await tcp::connect(host, port);
  co_return co_await client(std::move(transport), std::move(context), std::move(host), options);
}

/// Own host, credentials and one-shot session offer; connect TCP, then attempt policy-bound TLS resumption.
/// Handshake timeout excludes TCP/DNS setup. Borrowed state inside credentials remains caller-owned.
inline Task<TlsStream<TcpStream>> connect(
  TlsContext context,
  std::string host,
  u16 port,
  TlsSession session,
  TlsHandshakeOptions options = {})
{
  auto transport = co_await tcp::connect(host, port);
  co_return co_await client(std::move(transport), std::move(context), std::move(host), std::move(session), options);
}

} // namespace weave::tls
