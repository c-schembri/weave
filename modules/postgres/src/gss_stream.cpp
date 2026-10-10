#include "gss_stream.hpp"
#include <cstring>

namespace weave::pg::detail {

struct GssStream::Impl {
  TcpStream socket;
  GssSession session;
  SecretStorage<std::byte> plaintext;
  std::size_t consumed = 0;
  std::size_t maximum_plaintext = 0;
  std::size_t active = 0;
  bool reading = false;
  bool writing = false;
  bool finishing = false;
  bool eof = false;
  std::error_code error;

  explicit Impl(TcpStream transport) noexcept : socket(std::move(transport))
  {
  }

  ~Impl()
  {
    weave::detail::require(active == 0);
  }

  void poison(std::error_code reason) noexcept
  {
    if (!error)
      error = reason;
    static_cast<void>(socket.cancel());
  }

  struct Operation {
    Impl &stream;
    bool &direction;
    bool begun = false;
    bool completed = false;

    Result<void> begin()
    {
      if (stream.error)
        return std::unexpected(stream.error);
      if (direction || stream.finishing)
        return std::unexpected(make_error_code(Error::busy));
      direction = true;
      ++stream.active;
      begun = true;
      return {};
    }

    ~Operation()
    {
      if (!begun)
        return;
      if (!completed)
        stream.poison(std::make_error_code(std::errc::operation_canceled));
      direction = false;
      --stream.active;
    }
  };

  Task<void> handshake(GssContext &provider, std::string &host, GssOptions &options)
  {
    options.protect = true;
    auto token = co_await session.start(provider, host, Authentication::gss, options);
    for (unsigned step = 0; step < 64; ++step) {
      if (token.bytes.empty()) {
        if (!token.complete || !session.complete())
          co_await fail(Error::authentication);
        maximum_plaintext = session.plaintext_limit();
        if (!maximum_plaintext || maximum_plaintext > gss_record_limit)
          co_await fail(Error::resource_limit);
        co_return;
      }
      // Kerberos transport negotiation ends on a complete, empty initiator token.
      if (token.complete || token.bytes.size() > 65536 - 4)
        co_await fail(Error::authentication);
      SecretWriter packet;
      packet.integer(static_cast<u32>(token.bytes.size()));
      packet.raw(token.bytes);
      co_await socket.write_all(packet.bytes);

      std::array<std::byte, 4> header;
      co_await socket.read_exactly(header);
      Reader reader{header};
      const auto size = reader.integer();
      if (!size || size > 65536 - 4)
        co_await fail(Error::resource_limit);
      SecretStorage<std::byte> incoming(size);
      co_await socket.read_exactly(incoming);
      token = co_await session.next(incoming);
    }
    co_await fail(Error::authentication);
  }

  Task<std::size_t> read_data(std::span<std::byte> buffer)
  {
    // Validate the socket's execution binding even for buffered data and immediate EOF.
    co_await socket.read(std::span<std::byte>{});
    if (buffer.empty())
      co_return std::size_t{0};

    for (;;) {
      co_await cancellation_point();
      if (consumed != plaintext.size()) {
        const auto size = (std::min)(buffer.size(), plaintext.size() - consumed);
        std::memcpy(buffer.data(), plaintext.data() + consumed, size);
        clear_secret(plaintext.data() + consumed, size);
        consumed += size;
        co_return size;
      }
      if (eof)
        co_return std::size_t{0};

      std::array<std::byte, 4> header;
      std::size_t received = 0;
      while (received != header.size()) {
        const auto size = co_await socket.read(std::span{header}.subspan(received));
        if (!size) {
          if (received != 0)
            co_await fail(Error::protocol);
          // GSS has no authenticated close_notify; only record boundaries permit EOF.
          eof = true;
          co_return std::size_t{0};
        }
        received += size;
      }

      Reader reader{header};
      const auto size = reader.integer();
      if (!size || size > gss_record_limit)
        co_await fail(Error::resource_limit);
      SecretStorage<std::byte> record(size);
      co_await socket.read_exactly(record);
      plaintext = co_await session.unwrap(record);
      consumed = 0;
    }
  }

  Task<std::size_t> read(std::span<std::byte> buffer)
  {
    co_await cancellation_point();
    Operation operation{*this, reading};
    if (auto begun = operation.begin(); !begun)
      co_await fail(begun.error());
    auto result = co_await as_result(read_data(buffer));
    if (!result) {
      poison(result.error());
      co_await fail(result.error());
    }
    operation.completed = true;
    co_return *result;
  }

  Task<void> write_data(std::span<const std::byte> buffer)
  {
    co_await socket.write_all(std::span<const std::byte>{});
    SecretWriter packet;
    packet.bytes.reserve(gss_record_limit + 4);
    while (!buffer.empty()) {
      co_await cancellation_point();
      const auto size = (std::min)(buffer.size(), maximum_plaintext);
      auto record = co_await session.wrap(buffer.first(size));
      packet.bytes.clear();
      packet.integer(static_cast<u32>(record.size()));
      packet.raw(record);
      co_await socket.write_all(packet.bytes);
      buffer = buffer.subspan(size);
    }
  }

  Task<void> write_all(std::span<const std::byte> buffer)
  {
    co_await cancellation_point();
    Operation operation{*this, writing};
    if (auto begun = operation.begin(); !begun)
      co_await fail(begun.error());
    auto result = co_await as_result(write_data(buffer));
    if (!result) {
      poison(result.error());
      co_await fail(result.error());
    }
    operation.completed = true;
  }

  Task<void> finish()
  {
    if (active)
      co_await fail(Error::busy);
    Operation operation{*this, finishing};
    // Cleanup also works after terminal cancellation or synchronous socket close.
    finishing = true;
    ++active;
    operation.begun = true;
    co_await session.close();
    if (!error)
      error = make_error_code(Error::closed);
    if (auto result = socket.close(); !result)
      co_await fail(result.error());
    operation.completed = true;
  }
};

GssStream::GssStream(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

GssStream::GssStream(GssStream &&) noexcept = default;
GssStream &GssStream::operator=(GssStream &&) noexcept = default;
GssStream::~GssStream() = default;

GssStream::Impl &GssStream::state() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return *impl_;
}

Task<GssStream> GssStream::establish(
  TcpStream socket,
  GssContext provider,
  std::string host,
  GssOptions options,
  Diagnostic &diagnostic)
{
  auto stream = std::make_unique<Impl>(std::move(socket));
  auto result = co_await as_result(stream->handshake(provider, host, options));
  if (!result) {
    auto text = stream->session.diagnostic();
    if (!text.empty())
      diagnostic.fields.emplace_back('M', std::move(text));
    co_await stream->session.close();
    co_await fail(result.error());
  }
  co_return GssStream{std::move(stream)};
}

Task<std::size_t> GssStream::read(std::span<std::byte> buffer)
{
  return state().read(buffer);
}

Task<void> GssStream::write_all(std::span<const std::byte> buffer)
{
  return state().write_all(buffer);
}

Task<void> GssStream::finish()
{
  auto cleanup = state().finish();
  weave::detail::TaskAccess::bind(cleanup, {});
  return cleanup;
}

Result<void> GssStream::shutdown_send() noexcept
{
  auto &stream = state();
  if (stream.error)
    return std::unexpected(stream.error);
  if (stream.writing || stream.finishing)
    return std::unexpected(make_error_code(Error::busy));
  return stream.socket.shutdown_send();
}

Result<void> GssStream::close() noexcept
{
  if (!impl_)
    return {};
  if (impl_->active)
    return std::unexpected(std::make_error_code(std::errc::operation_in_progress));
  if (!impl_->error)
    impl_->error = make_error_code(Error::closed);
  // Like TLS, synchronous socket close retains native state until finish/destruction.
  return impl_->socket.close();
}

Result<void> GssStream::cancel() noexcept
{
  if (!impl_)
    return {};
  if (!impl_->error)
    impl_->error = std::make_error_code(std::errc::operation_canceled);
  return impl_->socket.cancel();
}

Task<GssTransport> gss_client(
  TcpStream socket,
  GssContext provider,
  std::string host,
  GssOptions options,
  GssEncryption mode,
  Diagnostic &diagnostic,
  bool require_channel_binding)
{
  weave::detail::require(mode == GssEncryption::prefer || mode == GssEncryption::require);
  Writer request;
  request.integer(8);
  request.integer(80877104);
  co_await socket.write_all(request.bytes);
  std::array<std::byte, 1> response;
  co_await socket.read_exactly(response);
  if (response[0] == std::byte{'N'}) {
    if (mode == GssEncryption::require)
      co_await fail(Error::authentication);
    co_return GssTransport{std::in_place_type<TcpStream>, std::move(socket)};
  }
  // Never parse/expose an unauthenticated ErrorResponse or retry after a failed proof.
  if (response[0] != std::byte{'G'} || require_channel_binding)
    co_await fail(Error::authentication);
  auto secured = co_await GssStream::establish(
    std::move(socket),
    std::move(provider),
    std::move(host),
    std::move(options),
    diagnostic);
  co_return GssTransport{std::in_place_type<GssStream>, std::move(secured)};
}

} // namespace weave::pg::detail
