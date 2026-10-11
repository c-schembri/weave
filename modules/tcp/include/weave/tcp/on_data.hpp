#pragma once

#include <weave/tcp/detail/on_data.hpp>

namespace weave::tcp {

/// Own a (TcpStream&, span<const byte>) -> Task<void> callback for serve().
/// Each client reuses one BufferSize-byte buffer after its callback completes; EOF has no callback.
/// Slices are borrowed through that callback only, not message boundaries. Do not retain them in
/// detached work. Shared callbacks can run concurrently; direct invocation requires a live adapter.
template <std::size_t BufferSize = 4096, detail::TcpDataCallback F>
  requires(BufferSize > 0)
auto on_data(F handler)
{
  return detail::TcpDataHandler<F, BufferSize>{std::move(handler)};
}

} // namespace weave::tcp
