#pragma once

#include <weave/task.hpp>
#include <span>
#include <cstddef>
#include <limits>

namespace weave {

template <class S>
concept ReadStream = requires(S &stream, std::span<std::byte> buffer) {
  { stream.read(buffer) } -> std::same_as<Task<std::size_t>>;
};

template <class S>
concept WriteStream = requires(S &stream, std::span<const std::byte> buffer) {
  { stream.write_all(buffer) } -> std::same_as<Task<void>>;
};

template <class S>
concept DuplexStream = ReadStream<S> && WriteStream<S>;

template <class S>
concept CancellableStream = DuplexStream<S> && requires(S &stream) {
  { stream.cancel() } -> std::same_as<Result<void>>;
  { stream.close() } -> std::same_as<Result<void>>;
};

namespace stream {

template <ReadStream S>
Task<void> read_exactly(S &source, std::span<std::byte> buffer)
{
  while (!buffer.empty()) {
    auto received = co_await source.read(buffer);
    detail::require(received <= buffer.size());
    if (!received)
      co_await fail(std::make_error_code(std::errc::connection_reset));

    buffer = buffer.subspan(received);
  }
}

template <ReadStream R, WriteStream W>
Task<std::size_t> copy(R &source, W &destination, std::span<std::byte> buffer)
{
  if (buffer.empty())
    co_await fail(std::make_error_code(std::errc::invalid_argument));

  std::size_t total = 0;

  while (auto received = co_await source.read(buffer)) {
    detail::require(received <= buffer.size());
    if (received > (std::numeric_limits<std::size_t>::max)() - total)
      co_await fail(std::errc::value_too_large);

    co_await destination.write_all(buffer.first(received));
    total += received;
  }

  co_return total;
}

} // namespace stream
} // namespace weave
