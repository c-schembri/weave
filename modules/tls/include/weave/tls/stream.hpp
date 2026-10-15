#pragma once

#include <weave/stream.hpp>
#include <weave/semaphore.hpp>
#include <weave/timer.hpp>
#include <weave/tls/detail/engine.hpp>
#include <weave/tcp/detail/read.hpp>
#include <array>
#include <algorithm>
#include <memory>
#include <type_traits>

namespace weave {

template <CancellableStream S>
class TlsStream;

namespace tls {

/// Own the transport, credentials, name and options before suspension; perform a timed client handshake.
/// Name is used for configured identity verification; cancellation after TLS advances is terminal.
template <CancellableStream S>
Task<TlsStream<S>> client(S transport, TlsContext context, std::string server_name, TlsHandshakeOptions options = {});
/// Client handshake with an owned one-shot session offer, bound to the same name/credential policy.
/// Resumption is opt-in and peer acceptance is not guaranteed; invalid policy offers are rejected.
template <CancellableStream S>
Task<TlsStream<S>> client(
  S transport,
  TlsContext context,
  std::string server_name,
  TlsSession session,
  TlsHandshakeOptions options = {});
/// Own transport, server credentials and options; complete a timed server handshake before returning.
template <CancellableStream S>
Task<TlsStream<S>> server(S transport, TlsContext context, TlsHandshakeOptions options = {});

} // namespace tls

/// Move-only TLS transport. One read and one write may run concurrently; shutdown is exclusive.
/// Borrowed buffers and this stream must survive cancellation drain. TLS-progress failures can
/// permanently poison the session; cancellation is not a reusable operation-level rollback.
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

  // Encrypted records include framing/authentication overhead beyond their 16 KiB plaintext.
  using WriteBuffer = std::array<std::byte, 17 * 1024>;
  using ReadBuffer = std::array<std::byte, 16384>;

  S transport_;
  detail::TlsEngine engine_;
  Semaphore send_{1};
  Semaphore receive_{1};
  std::unique_ptr<WriteBuffer> write_buffer_;
  std::unique_ptr<ReadBuffer> read_buffer_;

  TlsStream(S transport, detail::TlsEngine engine) noexcept
      : transport_(std::move(transport)), engine_(std::move(engine))
  {
  }

  // Keep synchronous failure cleanup out of every suspended I/O frame.
#if defined(_MSC_VER)
  __declspec(noinline)
#else
  [[gnu::noinline]]
#endif
  void poison(Error error) noexcept
  {
    engine_.fail(error);
    static_cast<void>(transport_.cancel());
  }

  Task<void> flush()
  {
    co_await cancellation_point();
    auto acquired = send_.try_acquire();
    if (!acquired && acquired.error() != std::errc::resource_unavailable_try_again)
      co_await fail(acquired.error());
    auto permit = acquired ? std::move(*acquired) : co_await send_.acquire();
    if (!write_buffer_) {
      write_buffer_.reset(new (std::nothrow) WriteBuffer);
      detail::require(write_buffer_ != nullptr);
    }
    auto &buffer = *write_buffer_;

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
    co_await cancellation_point();
    auto acquired = receive_.try_acquire();
    if (!acquired && acquired.error() != std::errc::resource_unavailable_try_again)
      co_await fail(acquired.error());
    auto permit = acquired ? std::move(*acquired) : co_await receive_.acquire();

    auto needed = engine_.needs_input(generation);
    if (!needed)
      co_await fail(needed.error());
    if (!*needed)
      co_return;

    if (!read_buffer_) {
      read_buffer_.reset(new (std::nothrow) ReadBuffer);
      detail::require(read_buffer_ != nullptr);
    }
    auto &buffer = *read_buffer_;
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

  Task<void> progress(detail::TlsStep step)
  {
    if (step.action == detail::TlsAction::failed) {
      poison(step.error);
      co_await fail(step.error);
    }

    // Never hold the transport write gate while waiting for transport input.
    auto sent = co_await as_result(flush());
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

  Result<bool> check_read_progress(const detail::TlsStep &step, const CancelToken &cancel) noexcept
  {
    if (step.action == detail::TlsAction::failed) {
      poison(step.error);
      return std::unexpected(step.error);
    }
    if (cancel.stop_requested()) {
      const auto error = std::make_error_code(std::errc::operation_canceled);
      poison(error);
      return std::unexpected(error);
    }

    auto pending = engine_.has_output();
    if (!pending)
      poison(pending.error());
    return pending;
  }

  Task<std::size_t> read_some(std::span<std::byte> buffer, bool acquire_operation = false)
  {
    co_await cancellation_point();

    detail::TlsOperationGuard guard{engine_, detail::TlsOperation::read};
    if (acquire_operation) {
      if (auto result = guard.begin(); !result)
        co_await fail(result.error());
    }

    if (buffer.empty())
      co_return co_await transport_.read(buffer);

    const auto cancel = co_await detail::GetCancellation{};
    for (;;) {
      u64 generation = 0;
      {
        auto step = engine_.read(buffer);
        // Reads with no outgoing records must not wait behind a backpressured writer.
        // Writes still join the send gate even if another flush took their records.
        auto pending = check_read_progress(step, cancel);
        if (!pending)
          co_await fail(pending.error());
        if (*pending) {
          auto sent = co_await as_result(flush());
          if (!sent) {
            poison(sent.error());
            co_await fail(sent.error());
          }
        }
        if (step.action == detail::TlsAction::eof)
          co_return std::size_t{0};
        if (step.action == detail::TlsAction::ready)
          co_return step.transferred;
        if (step.action != detail::TlsAction::input)
          continue;
        generation = step.generation;
      }
      {
        Result<void> received;
        {
          const auto &current = co_await detail::GetCancellationReference{};
          if (current.stop_requested())
            received = std::unexpected(std::make_error_code(std::errc::operation_canceled));
        }
        if (received) {
          // Release the receive permit before propagating a failure from this frame.
          auto acquired = receive_.try_acquire();
          if (!acquired) {
            if (acquired.error() == std::errc::resource_unavailable_try_again)
              received = co_await as_result(pull(generation));
            else
              received = std::unexpected(acquired.error());
          } else {
            auto permit = std::move(*acquired);
            auto needed = engine_.needs_input(generation);
            if (!needed) {
              received = std::unexpected(needed.error());
            } else if (*needed) {
              if (!read_buffer_) {
                read_buffer_.reset(new (std::nothrow) ReadBuffer);
                detail::require(read_buffer_ != nullptr);
              }
              auto &ciphertext = *read_buffer_;
              Result<std::size_t> read{std::size_t{0}};
              if constexpr (std::is_same_v<S, TcpStream>) {
                detail::TcpReadOperation operation{
                  transport_,
                  ciphertext,
                  detail::TcpReadOperation::Cancellation::snapshot};
                read = co_await operation;
              } else {
                read = co_await as_result(transport_.read(ciphertext));
              }
              if (!read) {
                poison(read.error());
                received = std::unexpected(read.error());
              } else {
                detail::require(*read <= ciphertext.size());
                received = engine_.input(std::span{ciphertext}.first(*read));
                if (!received)
                  poison(received.error());
              }
            }
          }
        }
        if (!received) {
          poison(received.error());
          co_await fail(received.error());
        }
      }
    }
  }

public:
  /// Move an idle stream; moving while an engine operation is active violates the contract.
  TlsStream(TlsStream &&other) noexcept
      : transport_(take_transport(other)), engine_(std::move(other.engine_)),
        write_buffer_(std::move(other.write_buffer_)), read_buffer_(std::move(other.read_buffer_))
  {
  }

  TlsStream(const TlsStream &) = delete;

  /// Destroy an idle stream without a graceful TLS shutdown; drain active operations first.
  ~TlsStream()
  {
    detail::require(!engine_.active());
  }

  /// Read decrypted bytes into borrowed storage; zero with a nonempty buffer means close_notify EOF.
  /// Raw transport EOF without close_notify is truncation, not authenticated shutdown.
  Task<std::size_t> read(std::span<std::byte> buffer)
  {
    return read_some(buffer, true);
  }

  /// Fill borrowed storage; premature authenticated EOF fails with connection_reset.
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

  /// Encrypt and flush all borrowed bytes; retain storage through completion/cancellation.
  /// TLS records do not define application message boundaries.
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
      if (step.action == detail::TlsAction::ready) {
        // Successful writes still join the transport barrier, even if its output was already taken.
        auto sent = co_await as_result(flush());
        if (!sent) {
          poison(sent.error());
          co_await fail(sent.error());
        }
      } else {
        co_await progress(step);
      }

      if (step.action == detail::TlsAction::ready) {
        detail::require(step.transferred > 0 && step.transferred <= chunk.size());
        buffer = buffer.subspan(step.transferred);
      }
    }
  }

  /// Send/flush close_notify without waiting for the peer. Unlike TCP half-close this performs I/O.
  Task<void> shutdown_send()
  {
    return finish(false);
  }

  /// Send close_notify and wait for the peer's reply; use timeout() if a bounded wait is needed.
  Task<void> shutdown()
  {
    return finish(true);
  }

  /// Mark TLS terminally cancelled and request transport cancellation; does not join active tasks.
  Result<void> cancel() noexcept
  {
    engine_.fail(std::make_error_code(std::errc::operation_canceled));

    return transport_.cancel();
  }

  /// Abort synchronously without close_notify; active engine operations return operation_in_progress.
  Result<void> close() noexcept
  {
    if (engine_.active())
      return std::unexpected(std::make_error_code(std::errc::operation_in_progress));

    engine_.fail(make_error_code(TlsError::closed));

    return transport_.close();
  }

  /// Negotiated TLS version; synchronous, no network I/O.
  TlsVersion version() const noexcept
  {
    return engine_.version();
  }

  /// Copy the selected ALPN label, or an empty string when none was negotiated.
  std::string negotiated_protocol() const
  {
    return engine_.alpn();
  }

  /// Copy the negotiated cipher name.
  std::string cipher() const
  {
    return engine_.cipher();
  }

  /// Whether this handshake actually resumed a session.
  bool session_reused() const noexcept
  {
    return engine_.session_reused();
  }

  /// Export a one-shot client session if enabled and available; TLS 1.3 tickets may arrive later.
  Result<TlsSession> session() const
  {
    return engine_.session();
  }

  /// Return an owning negotiated/verification snapshot, or a metadata extraction error.
  Result<TlsInfo> info() const
  {
    return engine_.info();
  }

  /// Export an authenticated peer identity only; an unverified certificate is not an identity.
  Result<TlsPeerIdentity> peer_identity() const
  {
    return engine_.peer_identity();
  }

  /// Export owned tls-server-end-point binding bytes for supported authenticated sessions.
  Result<std::vector<std::byte>> channel_binding() const
  {
    return engine_.channel_binding();
  }

  /// Synchronously derive owned secret bytes, borrowing label/context only during this call.
  /// An absent context differs from a present empty context; protect the returned key material.
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
