#pragma once

#include <weave/tls/context.hpp>
#include <weave/tls/stream.hpp>
#include <weave/tcp.hpp>

namespace weave::tls {

inline Task<TlsStream<TcpStream>> connect(const TlsContext &context, std::string host, u16 port)
{
  auto transport = co_await tcp::connect(host, port);
  co_return co_await client(std::move(transport), context, std::move(host));
}

} // namespace weave::tls
