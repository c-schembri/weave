#include <weave/tls.hpp>
#include <weave/timer.hpp>
#include <weave/channel.hpp>
#include "tls_certificates.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

using namespace std::chrono_literals;

enum class Mode {
  echo,
  idle,
  truncated,
  write_cancel,
  shutdown_cancel,
  busy,
  handshake_cancel
};

enum class Rejection {
  wrong_name,
  wrong_ip,
  untrusted,
  expired,
  alpn
};

struct Fragmented {
  weave::TcpStream socket;
  weave::Semaphore *write_gate = nullptr;

  weave::Task<std::size_t> read(std::span<std::byte> buffer)
  {
    co_return co_await socket.read(buffer.first((std::min)(buffer.size(), std::size_t{257})));
  }

  weave::Task<void> write_all(std::span<const std::byte> buffer)
  {
    if (write_gate) {
      auto permit = co_await write_gate->acquire();
    }

    while (!buffer.empty()) {
      auto size = (std::min)(buffer.size(), std::size_t{509});
      co_await socket.write_all(buffer.first(size));
      buffer = buffer.subspan(size);
    }
  }

  weave::Result<void> cancel() noexcept
  {
    return socket.cancel();
  }

  weave::Result<void> close() noexcept
  {
    return socket.close();
  }
};

static weave::Task<void> server(
  weave::TcpListener &listener,
  const weave::TlsContext &tls,
  Mode mode,
  weave::Channel<int> &release)
{
  auto raw = co_await listener.accept({.no_delay = true});
  if (mode == Mode::handshake_cancel) {
    co_await release.receive();
    co_return;
  }

  auto peer = co_await weave::tls::server(Fragmented{std::move(raw)}, tls);

  if (mode != Mode::echo && mode != Mode::truncated) {
    co_await release.receive();
    co_return;
  }

  if (mode == Mode::truncated)
    co_return;

  std::array<std::byte, 4096> buffer;
  while (auto count = co_await peer.read(buffer))
    co_await peer.write_all(std::span{buffer}.first(count));

  co_await peer.shutdown();
}

static weave::Task<void> duplicate(weave::TlsStream<Fragmented> &peer)
{
  co_await weave::sleep_for(1ms);

  std::array<std::byte, 1> buffer;
  auto result = co_await weave::as_result(peer.read(buffer));
  REQUIRE((!result && result.error() == std::errc::operation_in_progress));

  auto shutdown = co_await weave::as_result(peer.shutdown());
  REQUIRE((!shutdown && shutdown.error() == std::errc::operation_in_progress));

  auto close = peer.close();
  REQUIRE((!close && close.error() == std::errc::operation_in_progress));
  REQUIRE((peer.cancel()));
}

static weave::Task<void> client(
  weave::u16 port,
  const weave::TlsContext &tls,
  std::string name,
  Mode mode,
  weave::TlsVersion version)
{
  weave::Semaphore writes(1);
  auto raw = co_await weave::tcp::connect("127.0.0.1", port);
  auto handshake = weave::tls::client(Fragmented{std::move(raw), &writes}, tls, std::move(name));

  if (mode == Mode::handshake_cancel) {
    auto result = co_await weave::as_result(weave::timeout(5ms, std::move(handshake)));
    REQUIRE((!result && result.error() == std::errc::timed_out));
    co_return;
  }

  auto peer = co_await std::move(handshake);

  if (mode == Mode::busy) {
    std::array<std::byte, 1> buffer;
    auto read = [&]() -> weave::Task<void> {
      auto result = co_await weave::as_result(peer.read(buffer));
      REQUIRE((!result && result.error() == std::errc::operation_canceled));
    };

    co_await weave::when_all(read(), duplicate(peer));
    co_return;
  }

  if (mode == Mode::write_cancel || mode == Mode::shutdown_cancel) {
    auto held = writes.try_acquire();
    REQUIRE((held));

    std::array<std::byte, 8> buffer{};
    auto operation = mode == Mode::write_cancel ? peer.write_all(buffer) : peer.shutdown();
    auto result = co_await weave::as_result(weave::timeout(5ms, std::move(operation)));
    REQUIRE((!result && result.error() == std::errc::timed_out));

    auto retry = co_await weave::as_result(peer.write_all(buffer));
    REQUIRE((!retry && retry.error() == std::errc::operation_canceled));
    co_return;
  }

  if (mode == Mode::truncated) {
    std::array<std::byte, 1> buffer;
    co_await peer.read(buffer);
    co_return;
  }

  if (mode == Mode::idle) {
    std::array<std::byte, 1> buffer;
    auto result = co_await weave::as_result(weave::timeout(5ms, peer.read(buffer)));
    REQUIRE((!result && result.error() == std::errc::timed_out));

    auto retry = co_await weave::as_result(peer.read(buffer));
    REQUIRE((!retry && retry.error() == std::errc::operation_canceled));
    co_return;
  }

  // Cancellation before entry must not poison an established TLS stream.
  weave::CancelSource cancelled;
  cancelled.cancel();
  std::array<std::byte, 1> unused;
  auto pending = peer.read(unused);
  weave::detail::TaskAccess::bind(pending, cancelled.token());
  auto pre_cancelled = co_await weave::as_result(std::move(pending));
  REQUIRE((!pre_cancelled && pre_cancelled.error() == std::errc::operation_canceled));

  REQUIRE((co_await peer.read({})) == 0);
  co_await peer.write_all({});

  std::vector<std::byte> sent(65536, std::byte{42});
  std::vector<std::byte> received(sent.size());
  co_await weave::when_all(peer.read_exactly(received), peer.write_all(sent));
  REQUIRE((received == sent));
  REQUIRE((peer.version() == version));
  REQUIRE((peer.negotiated_protocol() == "echo"));

  co_await peer.shutdown_send();
  REQUIRE((co_await peer.read(unused)) == 0);
  co_await peer.shutdown();
}

static weave::Task<void> capture(
  weave::Task<void> task,
  weave::Result<void> &result,
  weave::Channel<int> *release = nullptr)
{
  result = co_await weave::as_result(std::move(task));

  if (release)
    release->close();
}

TEST_CASE("TLS adapters verify peers, preserve duplex IO, and drain cancellation")
{
  fixture::Certificates certificates;
  auto ctx = weave::Context::create();
  REQUIRE((ctx));

  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  for (auto version : versions) {
    INFO("version=", static_cast<int>(version));
    auto server_tls = weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .alpn = {"echo"},
        .min_version = version,
        .max_version = version});
    auto client_tls = weave::TlsContext::client(
      {.ca_file = certificates.ca, .alpn = {"echo"}, .min_version = version, .max_version = version});
    REQUIRE((server_tls && client_tls));

    const std::array modes{
      Mode::echo,
      Mode::idle,
      Mode::truncated,
      Mode::write_cancel,
      Mode::shutdown_cancel,
      Mode::busy,
      Mode::handshake_cancel};
    for (auto mode : modes) {
      INFO("mode=", static_cast<int>(mode));
      auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
      REQUIRE((listener));

      weave::Result<void> accepted;
      weave::Result<void> connected;
      weave::Channel<int> release(1);

      const auto name = version == weave::TlsVersion::tls12 ? "127.0.0.1" : "localhost";
      auto task = weave::when_all(
        capture(server(*listener, *server_tls, mode, release), accepted),
        capture(client(listener->local_port(), *client_tls, name, mode, version), connected, &release));
      REQUIRE((ctx->run(weave::timeout(5s, std::move(task)))));

      if (mode == Mode::truncated)
        REQUIRE((!connected && connected.error() == weave::TlsError::truncated));
      else
        REQUIRE((connected));

      if (mode == Mode::echo)
        REQUIRE((accepted));
    }
  }

  const std::array
    rejections{Rejection::wrong_name, Rejection::wrong_ip, Rejection::untrusted, Rejection::expired, Rejection::alpn};

  for (auto scenario : rejections) {
    INFO("rejection=", static_cast<int>(scenario));
    auto server_tls = weave::TlsContext::server(
      {.certificate_file = scenario == Rejection::expired ? certificates.expired : certificates.leaf,
        .private_key_file = certificates.private_key,
        .alpn = {"echo"}});
    auto client_tls = weave::TlsContext::client(
      {.ca_file = scenario == Rejection::untrusted ? certificates.untrusted : certificates.ca,
        .alpn = {scenario == Rejection::alpn ? "different" : "echo"}});
    REQUIRE((server_tls && client_tls));

    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE((listener));

    std::string name = "localhost";
    if (scenario == Rejection::wrong_name)
      name = "wrong.invalid";
    if (scenario == Rejection::wrong_ip)
      name = "127.0.0.2";

    weave::Result<void> accepted;
    weave::Result<void> connected;
    weave::Channel<int> release(1);
    auto task = weave::when_all(
      capture(server(*listener, *server_tls, Mode::echo, release), accepted),
      capture(client(listener->local_port(), *client_tls, name, Mode::echo, weave::TlsVersion::tls13), connected));

    REQUIRE((ctx->run(weave::timeout(5s, std::move(task)))));
    REQUIRE((!connected));
    if (scenario != Rejection::alpn)
      REQUIRE((connected.error() == weave::TlsError::certificate_verification));
  }
}

TEST_CASE("Context stop cancels and drains pending TLS transport reads")
{
  fixture::Certificates certificates;
  auto tls_server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  auto tls_client = weave::TlsContext::client({.ca_file = certificates.ca});
  REQUIRE(tls_server);
  REQUIRE(tls_client);

  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);

  weave::Result<void> accepted;
  weave::Result<void> connected;
  weave::Channel<int> release(1);

  auto stop = [&]() -> weave::Task<void> {
    co_await weave::sleep_for(1ms);
    ctx->request_stop();
  };

  auto read = [&]() -> weave::Task<void> {
    auto raw = co_await weave::tcp::connect("127.0.0.1", listener->local_port());
    auto peer = co_await weave::tls::client(std::move(raw), *tls_client, "localhost");
    std::array<std::byte, 1> buffer;

    auto pending_read = [&]() -> weave::Task<void> {
      auto result = co_await weave::as_result(peer.read(buffer));
      REQUIRE((!result && result.error() == std::errc::operation_canceled));
    };

    co_await weave::when_all(pending_read(), stop());
  };

  auto operation = weave::when_all(
    capture(server(*listener, *tls_server, Mode::idle, release), accepted),
    capture(read(), connected));
  auto stopped = ctx->run(weave::timeout(5s, std::move(operation)));
  REQUIRE_FALSE(stopped);
  CHECK(stopped.error() == std::errc::operation_canceled);
  CHECK(connected);
  REQUIRE_FALSE(accepted);
  CHECK(accepted.error() == std::errc::operation_canceled);
}

TEST_CASE("TLS setup validates options without starting asynchronous work")
{
  fixture::Certificates certificates;

  auto inverted = weave::TlsContext::client(
    {.min_version = weave::TlsVersion::tls13, .max_version = weave::TlsVersion::tls12});
  REQUIRE_FALSE(inverted);
  CHECK(inverted.error() == std::errc::invalid_argument);

  auto empty_protocol = weave::TlsContext::client({.alpn = {""}});
  REQUIRE_FALSE(empty_protocol);
  CHECK(empty_protocol.error() == std::errc::invalid_argument);

  auto nul_path = weave::TlsContext::client({.ca_file = std::string{"ca\0other", 8}});
  REQUIRE_FALSE(nul_path);
  CHECK(nul_path.error() == std::errc::invalid_argument);

  auto missing_key = weave::TlsContext::server({.certificate_file = certificates.leaf});
  REQUIRE_FALSE(missing_key);
  CHECK(missing_key.error() == std::errc::invalid_argument);

  auto mismatched = weave::TlsContext::server(
    {.certificate_file = certificates.ca, .private_key_file = certificates.private_key});
  CHECK_FALSE(mismatched);
}

static void transfer(weave::detail::TlsEngine &source, weave::detail::TlsEngine &destination)
{
  std::array<std::byte, 32768> buffer;

  for (;;) {
    auto count = source.output(buffer);
    REQUIRE(count);
    if (!*count)
      return;

    REQUIRE(destination.input(std::span{buffer}.first(*count)));
  }
}

TEST_CASE("TLS shutdown consumes buffered peer close_notify before requesting more input")
{
  fixture::Certificates certificates;
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};

  for (auto version : versions) {
    auto server_credentials = weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .min_version = version,
        .max_version = version});
    auto client_credentials = weave::TlsContext::client(
      {.ca_file = certificates.ca, .min_version = version, .max_version = version});
    REQUIRE(server_credentials);
    REQUIRE(client_credentials);

    auto server = weave::detail::TlsEngine::create(*server_credentials, true, "");
    auto client = weave::detail::TlsEngine::create(*client_credentials, false, "localhost");
    REQUIRE(server);
    REQUIRE(client);

    bool server_ready = false;
    bool client_ready = false;
    for (int step = 0; step < 100 && (!server_ready || !client_ready); ++step) {
      if (!client_ready)
        client_ready = client->handshake().action == weave::detail::TlsAction::ready;
      transfer(*client, *server);

      if (!server_ready)
        server_ready = server->handshake().action == weave::detail::TlsAction::ready;
      transfer(*server, *client);
    }
    REQUIRE(server_ready);
    REQUIRE(client_ready);

    REQUIRE(server->shutdown(false).action == weave::detail::TlsAction::ready);
    transfer(*server, *client);

    // No more transport bytes will arrive. The next attempt must consume existing input.
    REQUIRE(client->shutdown(true).action == weave::detail::TlsAction::output);
    CHECK(client->shutdown(true).action == weave::detail::TlsAction::ready);
  }
}
