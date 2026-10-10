#include <weave/tls.hpp>
#include <weave/scope.hpp>
#include <weave/channel.hpp>
#include "tls_certificates.hpp"
#include <doctest/doctest.h>

using namespace std::chrono_literals;

struct Control {
  bool enabled = false;
  weave::Channel<int> release{1};
  weave::Channel<int> entered{1};
  weave::Channel<int> respond{1};
};

struct Gated {
  weave::TcpStream socket;
  Control *control;

  weave::Task<std::size_t> read(std::span<std::byte> buffer)
  {
    co_return co_await socket.read(buffer);
  }

  weave::Task<void> write_all(std::span<const std::byte> buffer)
  {
    if (control->enabled) {
      co_await control->entered.send(1);
      co_await control->release.receive();
    }
    co_await socket.write_all(buffer);
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

static weave::Task<void> server(weave::TcpListener &listener, const weave::TlsContext &tls, Control &control)
{
  auto socket = co_await listener.accept({.no_delay = true});
  auto peer = co_await weave::tls::server(std::move(socket), tls);
  co_await control.respond.receive();
  const std::array response{std::byte{42}};
  co_await peer.write_all(response);
  std::array<std::byte, 1> request;
  co_await peer.read_exactly(request);
  if (request.front() != std::byte{37})
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> receive(weave::TlsStream<Gated> &peer, Control &control)
{
  co_await control.entered.receive();
  co_await control.respond.send(1);
  std::array<std::byte, 1> response;
  co_await peer.read_exactly(response);
  if (response.front() != std::byte{42})
    co_await weave::fail(std::errc::bad_message);
  co_await control.release.send(1);
}

static weave::Task<void> client(weave::u16 port, const weave::TlsContext &tls, Control &control)
{
  auto socket = co_await weave::tcp::connect("127.0.0.1", port);
  auto peer = co_await weave::tls::client(Gated{std::move(socket), &control}, tls, "localhost");
  control.enabled = true;
  const std::array request{std::byte{37}};
  co_await weave::when_all(peer.write_all(request), receive(peer, control));
}

static weave::Task<void> exchange(const weave::TlsContext &server_tls, const weave::TlsContext &client_tls)
{
  Control control;
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  co_await weave::when_all(server(listener, server_tls, control), client(listener.local_port(), client_tls, control));
}

TEST_CASE("TLS reads progress while an unrelated transport write is backpressured")
{
  fixture::Certificates certificates;
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  const std::array versions{weave::TlsVersion::tls12, weave::TlsVersion::tls13};
  for (auto version : versions) {
    auto server_tls = weave::TlsContext::server(
      {.certificate_file = certificates.leaf,
        .private_key_file = certificates.private_key,
        .min_version = version,
        .max_version = version});
    auto client_tls = weave::TlsContext::client(
      {.ca_file = certificates.ca, .min_version = version, .max_version = version});
    INFO("version=", static_cast<unsigned>(version));
    REQUIRE(server_tls);
    REQUIRE(client_tls);

    auto result = ctx->run(weave::timeout(5s, exchange(*server_tls, *client_tls)));
    const auto message = result ? std::string{} : result.error().message();
    REQUIRE_MESSAGE(result.has_value(), message);
  }
}
