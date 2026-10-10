#pragma once

#include <weave/stream.hpp>
#include <weave/semaphore.hpp>
#include <weave/timer.hpp>
#include <weave/tls/detail/engine.hpp>
#include <array>
#include <algorithm>

namespace weave {

template <CancellableStream S>
class TlsStream;

namespace tls {

template <CancellableStream S>
Task<TlsStream<S>> client(S transport, TlsContext context, std::string server_name, TlsHandshakeOptions options = {});
template <CancellableStream S>
Task<TlsStream<S>> client(
  S transport,
  TlsContext context,
  std::string server_name,
  TlsSession session,
  TlsHandshakeOptions options = {});
template <CancellableStream S>
Task<TlsStream<S>> server(S transport, TlsContext context, TlsHandshakeOptions options = {});

} // namespace tls

template <CancellableStream S>
class TlsStream {
  static_assert(std::is_nothrow_move_constructible_v<S>);
  static_assert(std::is_nothrow_destructible_v<S>);

  template <CancellableStream T>
  friend Task<TlsStream<T>> tls::client(T, TlsContext, std::string, TlsHandshakeOptions);
  template <CancellableStream T>
  friend Task<TlsStream<T>> tls::client(T, TlsContext, std::string, TlsSession, TlsHandshakeOptions);
  template <CancellableStream T>
  friend Task<TlsStream<T>> tls::server(T, TlsContext, TlsHandshakeOptions);

  S transport_;
  detail::TlsEngine engine_;
  Semaphore send_{1};
  Semaphore receive_{1};

  TlsStream(S transport, detail::TlsEngine engine) noexcept
      : transport_(std::move(transport)), engine_(std::move(engine))
  {
  }

  void poison(Error error) noexcept
  {
    engine_.fail(error);
    static_cast<void>(transport_.cancel());
  }

  Task<void> flush(bool send_barrier)
  {
    if (!send_barrier) {
      auto pending = engine_.has_output();
      if (!pending)
        co_await fail(pending.error());
      if (!*pending)
        co_return;
    }

    auto permit = co_await send_.acquire();
    std::array<std::byte, 16384> buffer;

    for (;;) {
      auto count = engine_.output(buffer);
      if (!count)
        co_await fail(count.error());
      if (!*count)
        co_return;

      auto result = co_await as_result(transport_.write_all(std::span{buffer}.first(*count)));
      if (!result) {
        poison(result.error());
        co_await fail(result.error());
      }
    }
  }

  Task<void> pull(u64 generation)
  {
    auto permit = co_await receive_.acquire();

    auto needed = engine_.needs_input(generation);
    if (!needed)
      co_await fail(needed.error());
    if (!*needed)
      co_return;

    std::array<std::byte, 16384> buffer;
    auto received = co_await as_result(transport_.read(buffer));
    if (!received) {
      poison(received.error());
      co_await fail(received.error());
    }

    detail::require(*received <= buffer.size());

    auto result = engine_.input(std::span{buffer}.first(*received));
    if (!result) {
      poison(result.error());
      co_await fail(result.error());
    }
  }

  Task<void> progress(detail::TlsStep step, bool send_barrier = true)
  {
    if (step.action == detail::TlsAction::failed) {
      poison(step.error);
      co_await fail(step.error);
    }

    // Never hold the transport write gate while waiting for transport input.
    auto sent = co_await as_result(flush(send_barrier));
    if (!sent) {
      poison(sent.error());
      co_await fail(sent.error());
    }

    if (step.action == detail::TlsAction::input) {
      auto received = co_await as_result(pull(step.generation));
      if (!received) {
        poison(received.error());
        co_await fail(received.error());
      }
    }
  }

  Task<void> handshake()
  {
    for (;;) {
      auto step = engine_.handshake();
      co_await progress(step);

      if (step.action == detail::TlsAction::ready)
        co_return;
    }
  }

  Task<void> finish(bool wait_peer)
  {
    co_await cancellation_point();

    detail::TlsOperationGuard guard{engine_, detail::TlsOperation::exclusive};
    if (auto result = guard.begin(); !result)
      co_await fail(result.error());

    for (;;) {
      auto step = engine_.shutdown(wait_peer);
      co_await progress(step);

      if (step.action == detail::TlsAction::ready)
        co_return;
    }
  }

  Task<std::size_t> read_some(std::span<std::byte> buffer)
  {
    if (buffer.empty())
      co_return co_await transport_.read(buffer);

    for (;;) {
      auto step = engine_.read(buffer);
      // Reads with no outgoing records must not wait behind a backpressured writer.
      // Writes still join the send gate even if another flush took their records.
      co_await progress(step, false);

      if (step.action == detail::TlsAction::eof)
        co_return std::size_t{0};
      if (step.action == detail::TlsAction::ready)
        co_return step.transferred;
    }
  }

public:
  TlsStream(TlsStream &&other) noexcept : transport_(take_transport(other)), engine_(std::move(other.engine_))
  {
  }

  TlsStream(const TlsStream &) = delete;

  ~TlsStream()
  {
    detail::require(!engine_.active());
  }

  Task<std::size_t> read(std::span<std::byte> buffer)
  {
    co_await cancellation_point();

    detail::TlsOperationGuard guard{engine_, detail::TlsOperation::read};
    if (auto result = guard.begin(); !result)
      co_await fail(result.error());

    co_return co_await read_some(buffer);
  }

  Task<void> read_exactly(std::span<std::byte> buffer)
  {
    co_await cancellation_point();

    detail::TlsOperationGuard guard{engine_, detail::TlsOperation::read};
    if (auto result = guard.begin(); !result)
      co_await fail(result.error());

    if (buffer.empty()) {
      co_await read_some(buffer);
      co_return;
    }

    while (!buffer.empty()) {
      auto received = co_await read_some(buffer);
      if (!received)
        co_await fail(std::make_error_code(std::errc::connection_reset));

      buffer = buffer.subspan(received);
    }
  }

  Task<void> write_all(std::span<const std::byte> buffer)
  {
    co_await cancellation_point();

    detail::TlsOperationGuard guard{engine_, detail::TlsOperation::write};
    if (auto result = guard.begin(); !result)
      co_await fail(result.error());

    if (buffer.empty()) {
      co_await transport_.write_all(buffer);
      co_return;
    }

    while (!buffer.empty()) {
      auto chunk = buffer.first((std::min)(buffer.size(), std::size_t{16384}));
      auto step = engine_.write(chunk);
      co_await progress(step);

      if (step.action == detail::TlsAction::ready) {
        detail::require(step.transferred > 0 && step.transferred <= chunk.size());
        buffer = buffer.subspan(step.transferred);
      }
    }
  }

  Task<void> shutdown_send()
  {
    return finish(false);
  }

  Task<void> shutdown()
  {
    return finish(true);
  }

  Result<void> cancel() noexcept
  {
    engine_.fail(std::make_error_code(std::errc::operation_canceled));

    return transport_.cancel();
  }

  Result<void> close() noexcept
  {
    if (engine_.active())
      return std::unexpected(std::make_error_code(std::errc::operation_in_progress));

    engine_.fail(make_error_code(TlsError::closed));

    return transport_.close();
  }

  TlsVersion version() const noexcept
  {
    return engine_.version();
  }

  std::string negotiated_protocol() const
  {
    return engine_.alpn();
  }

  std::string cipher() const
  {
    return engine_.cipher();
  }

  bool session_reused() const noexcept
  {
    return engine_.session_reused();
  }

  Result<TlsSession> session() const
  {
    return engine_.session();
  }

  Result<TlsInfo> info() const
  {
    return engine_.info();
  }

  Result<TlsPeerIdentity> peer_identity() const
  {
    return engine_.peer_identity();
  }

  Result<std::vector<std::byte>> channel_binding() const
  {
    return engine_.channel_binding();
  }

  Result<std::vector<std::byte>> export_keying_material(
    std::string_view label,
    std::size_t size,
    std::optional<std::span<const std::byte>> context = std::nullopt) const
  {
    return engine_.export_keying_material(label, size, context);
  }

private:
  static S take_transport(TlsStream &other) noexcept
  {
    detail::require(!other.engine_.active());
    return std::move(other.transport_);
  }
};

namespace tls {

template <CancellableStream S>
Task<TlsStream<S>> client(S transport, TlsContext context, std::string server_name, TlsHandshakeOptions options)
{
  co_await cancellation_point();
  if (options.timeout <= std::chrono::milliseconds{0} || options.timeout > std::chrono::hours{24})
    co_await fail(std::errc::invalid_argument);

  auto engine = detail::TlsEngine::create(
    context,
    false,
    server_name,
    nullptr,
    options.required_protocol,
    options.server_name_indication,
    options.client_certificate);
  if (!engine)
    co_await fail(engine.error());

  TlsStream<S> stream{std::move(transport), std::move(*engine)};
  co_await timeout(options.timeout, stream.handshake());

  co_return std::move(stream);
}

template <CancellableStream S>
Task<TlsStream<S>> client(
  S transport,
  TlsContext context,
  std::string server_name,
  TlsSession session,
  TlsHandshakeOptions options)
{
  co_await cancellation_point();
  if (options.timeout <= std::chrono::milliseconds{0} || options.timeout > std::chrono::hours{24})
    co_await fail(std::errc::invalid_argument);

  auto engine = detail::TlsEngine::create(
    context,
    false,
    server_name,
    &session,
    options.required_protocol,
    options.server_name_indication,
    options.client_certificate);
  if (!engine)
    co_await fail(engine.error());

  TlsStream<S> stream{std::move(transport), std::move(*engine)};
  co_await timeout(options.timeout, stream.handshake());

  co_return std::move(stream);
}

template <CancellableStream S>
Task<TlsStream<S>> server(S transport, TlsContext context, TlsHandshakeOptions options)
{
  co_await cancellation_point();
  if (options.timeout <= std::chrono::milliseconds{0} || options.timeout > std::chrono::hours{24})
    co_await fail(std::errc::invalid_argument);

  auto engine = detail::TlsEngine::create(context, true, {}, nullptr, options.required_protocol);
  if (!engine)
    co_await fail(engine.error());

  TlsStream<S> stream{std::move(transport), std::move(*engine)};
  co_await timeout(options.timeout, stream.handshake());

  co_return std::move(stream);
}

} // namespace tls
} // namespace weave
