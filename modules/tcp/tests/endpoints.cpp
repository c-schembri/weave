#include <doctest/doctest.h>
#include <weave/tcp.hpp>
#include <weave/timer.hpp>
#include <weave/resolve.hpp>
#include <array>

static weave::Task<void> endpoint_server(weave::TcpListener &listener)
{
  auto stream = co_await listener.accept({.no_delay = true});
  auto local = stream.local_endpoint();
  auto peer = stream.peer_endpoint();
  if (!local || !peer)
    co_await weave::fail(std::errc::io_error);
  CHECK(local->port == listener.local_port());
  CHECK(local->address.is_v6());
  CHECK(peer->address.is_v6());
  CHECK(peer->port != 0);
  std::array<std::byte, 73> buffer{};
  while (auto count = co_await stream.read(buffer))
    co_await stream.write_all(std::span(buffer).first(count));
  CHECK(stream.close());
  CHECK_FALSE(stream.local_endpoint());
  CHECK_FALSE(stream.peer_endpoint());
}

static weave::Task<void> endpoint_client(weave::Endpoint endpoint, bool dns)
{
  std::vector<weave::Endpoint> endpoints{endpoint};
  if (dns)
    endpoints = co_await weave::resolve("localhost", endpoint.port, {.family = endpoint.address.family()});
  auto stream = co_await weave::tcp::connect(std::move(endpoints));
  auto local = stream.local_endpoint();
  auto peer = stream.peer_endpoint();
  if (!local || !peer)
    co_await weave::fail(std::errc::io_error);
  CHECK(peer->port == endpoint.port);
  CHECK(local->port != 0);
  std::array<std::byte, 8193> sent{};
  std::array<std::byte, 8193> received{};
  for (std::size_t i = 0; i < sent.size(); ++i)
    sent[i] = std::byte(i % 251);
  co_await stream.write_all(sent);
  co_await stream.read_exactly(received);
  CHECK(sent == received);
  auto shutdown = stream.shutdown_send();
  if (!shutdown)
    co_await weave::fail(shutdown.error());
  CHECK(co_await stream.read(received) == 0);
}

TEST_CASE("IPv6 and dual-stack listeners preserve endpoints, partial payloads and EOF")
{
  constexpr std::array completion_modes{false, true};
  constexpr std::array dual_stack_modes{false, true};
  constexpr std::array resolution_modes{false, true};

  for (bool skip : completion_modes) {
    for (bool dual : dual_stack_modes) {
      auto ctx = weave::Context::create({.skip_successful_completions = skip});
      REQUIRE(ctx);
      auto listener = weave::tcp::listen(
        *ctx,
        {dual ? weave::IpAddress::any_v6() : weave::IpAddress::loopback_v6(), 0},
        {.ipv6_only = !dual});
      REQUIRE(listener);
      auto endpoint = listener->local_endpoint();
      CHECK(endpoint.address.is_v6());
      CHECK(endpoint.port != 0);
      auto destination = endpoint;
      destination.address = dual ? weave::IpAddress::loopback_v4() : weave::IpAddress::loopback_v6();
      for (bool dns : resolution_modes) {
        auto result = ctx->run(
          weave::timeout(
            std::chrono::seconds(5),
            weave::when_all(endpoint_server(*listener), endpoint_client(destination, dns))));
        CAPTURE(result ? 0 : result.error().value());
        REQUIRE(result);
      }
      CHECK(listener->close());
      CHECK(listener->local_endpoint() == endpoint);
      auto moved = std::move(*listener);
      CHECK(moved.local_endpoint() == endpoint);
      CHECK(listener->local_port() == 0);
      CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    }
  }
}

TEST_CASE("IPv6 literal listeners default to v6-only and connections try owned endpoints in order")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "::", 0);
  REQUIRE(listener);
  const auto port = listener->local_port();
  CHECK_FALSE(ctx->run(weave::tcp::connect(weave::Endpoint{weave::IpAddress::loopback_v4(), port})));
  auto connect = [&]() -> weave::Task<void> {
    std::vector<weave::Endpoint> endpoints{
      {weave::IpAddress::loopback_v4(), 0},
      {weave::IpAddress::loopback_v6(), port}};
    auto stream = co_await weave::tcp::connect(*ctx, std::move(endpoints));
    auto peer = stream.peer_endpoint();
    if (!peer)
      co_await weave::fail(peer.error());
    CHECK(peer->address == weave::IpAddress::loopback_v6());
    CHECK(peer->port == port);
  };
  CHECK(ctx->run(connect()));
  auto accepted = ctx->run(listener->accept());
  REQUIRE(accepted);
  CHECK_FALSE(ctx->run(weave::tcp::connect(std::vector<weave::Endpoint>{})));
  CHECK_FALSE(ctx->run(weave::tcp::connect(std::string("localhost\0ignored", 17), port)));

  weave::CancelSource stop;
  stop.cancel();
  auto job = ctx->spawn(
    weave::tcp::connect(std::vector<weave::Endpoint>{{weave::IpAddress::loopback_v6(), port}}),
    {.cancel = stop.token()});
  REQUIRE(job);
  auto result = ctx->run(std::move(*job).as_task());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}
