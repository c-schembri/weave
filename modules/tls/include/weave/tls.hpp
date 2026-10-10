#pragma once

#include <weave/tls/context.hpp>
#include <weave/tls/stream.hpp>
#include <weave/tcp.hpp>

namespace weave::tls {

inline Task<TlsStream<TcpStream>> connect(
  TlsContext context,
  std::string host,
  u16 port,
  TlsHandshakeOptions options = {})
{
  auto transport = co_await tcp::connect(host, port);
  co_return co_await client(std::move(transport), std::move(context), std::move(host), options);
}

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
