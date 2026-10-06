#pragma once

#include <weave/tcp/detail/on_data.hpp>

namespace weave::tcp {

// Owns a callback for serve(). Each client gets a reusable BufferSize-byte receive buffer.
// Data is borrowed until the callback's Task completes; TCP chunks are not message boundaries.
template <std::size_t BufferSize = 4096, detail::TcpDataCallback F>
  requires(BufferSize > 0)
auto on_data(F handler)
{
  return detail::TcpDataHandler<F, BufferSize>{std::move(handler)};
}

} // namespace weave::tcp
