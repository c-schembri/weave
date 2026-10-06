#pragma once

#include <weave/tcp/stream.hpp>
#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <span>
#include <type_traits>
#include <utility>

namespace weave::detail {

template <class F>
concept TcpDataCallback = std::invocable<F &, TcpStream &, std::span<const std::byte>> &&
  std::same_as<std::invoke_result_t<F &, TcpStream &, std::span<const std::byte>>, Task<void>>;

template <TcpDataCallback F, std::size_t BufferSize>
struct TcpDataHandler {
  static_assert(BufferSize > 0);
  F handler;

  Task<void> operator()(TcpStream client) &
  {
    std::array<std::byte, BufferSize> buffer;
    while (auto received = co_await client.read(buffer))
      co_await std::invoke(handler, client, std::span<const std::byte>{buffer}.first(received));
  }
};

} // namespace weave::detail
