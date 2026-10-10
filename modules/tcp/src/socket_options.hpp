#pragma once

#include <weave/tcp/stream.hpp>
#include <limits>

namespace weave::detail {

inline bool valid_socket_timeout(std::chrono::milliseconds timeout) noexcept
{
  return timeout.count() >= 0 && timeout.count() <= std::numeric_limits<int>::max();
}

inline bool valid_keep_alive(const TcpKeepAliveOptions &options) noexcept
{
  constexpr auto maximum = std::numeric_limits<int>::max();
  return options.idle.count() >= 0 && options.idle.count() <= maximum && options.interval.count() >= 0 &&
    options.interval.count() <= maximum && options.probes <= static_cast<u32>(maximum);
}

} // namespace weave::detail
