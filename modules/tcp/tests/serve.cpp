#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/tcp/serve.hpp>
#include <weave/timer.hpp>
#include "echo_peer.hpp"
#include "backend.hpp"
#include <memory>

using namespace std::chrono_literals;

struct ServerState {
  int entered = 0;
  int active = 0;
  int destroyed = 0;
  int handler_destroyed = 0;
  int failed = 0;
  int cancelled = 0;
};

struct HandlerLifetime {
  ServerState &state;

  ~HandlerLifetime()
  {
    CHECK(state.active == 0);
    ++state.handler_destroyed;
  }
};

struct ClientLifetime {
  ServerState &state;

  explicit ClientLifetime(ServerState &state) : state(state)
  {
    ++state.entered;
    ++state.active;
  }

  ~ClientLifetime()
  {
    CHECK(state.handler_destroyed == 0);
    --state.active;
    ++state.destroyed;
  }
};

struct EchoHandler {
  ServerState &state;
  std::unique_ptr<HandlerLifetime> lifetime;

  weave::Task<void> operator()(weave::TcpStream client)
  {
    ClientLifetime active{state};
    std::array<std::byte, 4096> buffer;
    for (;;) {
      auto received = co_await client.read(buffer);
      if (received == 0)
        co_return;
      if (buffer[0] == std::byte{255})
        co_await weave::fail(std::errc::io_error);
      co_await client.write_all(std::span{buffer}.first(received));
    }
  }
};

static weave::Task<void> discard(weave::TcpStream)
{
  co_return;
}

static weave::Task<void> roundtrip(weave::u16 port)
{
  auto client = co_await weave::tcp::connect("127.0.0.1", port);
  std::array<std::byte, 8193> sent{}, received{};
  for (std::size_t i = 0; i < sent.size(); ++i)
    sent[i] = static_cast<std::byte>(i % 251);
  co_await client.write_all(sent);
  if (auto status = client.shutdown_send(); !status)
    co_await weave::fail(status.error());
  co_await client.read_exactly(received);
  CHECK(sent == received);
  CHECK(co_await client.read(std::span{received}.first(1)) == 0);
}

TEST_CASE("TCP serve is lazy and reports setup errors without invoking client handlers")
{
  static_assert(std::is_same_v<
    decltype(weave::tcp::serve("127.0.0.1", 0, {.backlog = 512, .no_delay = true}, discard)),
    weave::Task<void>>);
  ServerState state;
  {
    auto unused = weave::tcp::serve("127.0.0.1", 0, {}, EchoHandler{state, std::make_unique<HandlerLifetime>(state)});
    CHECK(state.entered == 0);
    CHECK(state.handler_destroyed == 0);
  }
  CHECK(state.handler_destroyed == 1);

  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int observed = 0;
  auto invalid = ctx->run(weave::tcp::serve("not-an-ip", 0, {}, discard, [&](weave::Error) noexcept {
    ++observed;
  }));
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == std::errc::invalid_argument);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  CHECK_FALSE(ctx->run(weave::tcp::serve("127.0.0.1", listener->local_port(), {}, discard)));
  CHECK(observed == 0);
  REQUIRE(listener->close());

  auto closed = ctx->run(weave::tcp::serve(*listener, {}, discard));
  CHECK_FALSE(closed);
  CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.empty());
}

TEST_CASE("TCP serve isolates client failures, configures sockets and drains cancellation")
{
  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0, 512);
    REQUIRE(listener);
    auto idle = support::connect(listener->local_port());
    ServerState state;
    auto server = ctx->spawn(
      weave::tcp::serve(
        *listener,
        {.no_delay = true},
        EchoHandler{state, std::make_unique<HandlerLifetime>(state)},
        [&](weave::Error error) noexcept {
          if (error == std::errc::operation_canceled)
            ++state.cancelled;
          else {
            CHECK(error == std::errc::io_error);
            ++state.failed;
          }
        }));
    REQUIRE(server);

    auto exercise = [&]() -> weave::Task<void> {
      while (state.entered == 0)
        co_await ctx->yield();
      auto &handles = weave::detail::IoAccess::state(*ctx).registered_handles_;
      int connected = 0;
      // The registry also holds the listener and the next AcceptEx's temporary socket.
      for (auto handle : handles) {
        sockaddr_in peer{};
        support::SocketLength peer_size = sizeof(peer);
        if (getpeername(handle, reinterpret_cast<sockaddr *>(&peer), &peer_size) != 0)
          continue;
        int enabled = 0;
        support::SocketLength size = sizeof(enabled);
        REQUIRE(getsockopt(handle, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char *>(&enabled), &size) == 0);
        CHECK(enabled == 1);
        ++connected;
      }
      CHECK(connected == 1);

      co_await weave::when_all(
        roundtrip(listener->local_port()),
        roundtrip(listener->local_port()),
        roundtrip(listener->local_port()));
      auto failing = co_await weave::tcp::connect("127.0.0.1", listener->local_port());
      std::array message{std::byte{255}};
      co_await failing.write_all(message);
      while (state.failed == 0)
        co_await ctx->yield();
      REQUIRE(failing.close());
      co_await roundtrip(listener->local_port());

      server->cancel();
      auto stopped = co_await weave::as_result(std::move(*server));
      REQUIRE_FALSE(stopped);
      CHECK(stopped.error() == std::errc::operation_canceled);
      CHECK(state.active == 0);
      CHECK(state.destroyed == 6);
      CHECK(state.handler_destroyed == 1);
      CHECK(state.failed == 1);
      CHECK(state.cancelled == 1);
    };
    auto result = ctx->run(weave::timeout(5s, exercise()));
    CHECK(support::close_socket(idle) == 0);
    REQUIRE(result);
    REQUIRE(listener->close());
    CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.empty());
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}

TEST_CASE("TCP serve retains a temporary coroutine lambda and owns its endpoint listener")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto reserved = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(reserved);
  auto port = reserved->local_port();
  REQUIRE(reserved->close());
  ServerState state;
  auto server = ctx->spawn(
    weave::tcp::serve(
      "127.0.0.1",
      port,
      {.backlog = 512, .no_delay = true},
      [lifetime = std::make_unique<HandlerLifetime>(state), &state](weave::TcpStream client) -> weave::Task<void> {
        CHECK(lifetime != nullptr);
        ClientLifetime active{state};
        std::array<std::byte, 1> buffer{};
        co_await client.read(buffer);
        FAIL("Cancelled client resumed its body");
      }));
  REQUIRE(server);
  auto exercise = [&]() -> weave::Task<void> {
    while (weave::detail::IoAccess::state(*ctx).registered_handles_.empty())
      co_await ctx->yield();
    auto client = co_await weave::tcp::connect("127.0.0.1", port);
    while (state.entered == 0)
      co_await ctx->yield();
    server->cancel();
    auto result = co_await weave::as_result(std::move(*server));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
    CHECK(state.destroyed == 1);
    CHECK(state.handler_destroyed == 1);
    REQUIRE(client.close());
  };
  REQUIRE(ctx->run(weave::timeout(5s, exercise())));
  CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.empty());
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  // The server owned and closed its listener before returning, without stopping the Context.
  CHECK(weave::tcp::listen(*ctx, "127.0.0.1", port));
}

TEST_CASE("TCP serve drains clients before reporting a native accept failure")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto idle = support::connect(listener->local_port());
  ServerState state;
  auto server = ctx->spawn(
    weave::tcp::serve(*listener, {}, EchoHandler{state, std::make_unique<HandlerLifetime>(state)}));
  REQUIRE(server);
  auto exercise = [&]() -> weave::Task<void> {
    while (state.entered == 0)
      co_await ctx->yield();
    REQUIRE(listener->cancel());
    auto result = co_await weave::as_result(std::move(*server));
    REQUIRE_FALSE(result);
    CHECK(result.error().value() == support::native_cancelled);
    CHECK(state.active == 0);
    CHECK(state.destroyed == 1);
    CHECK(state.handler_destroyed == 1);
  };
  auto result = ctx->run(weave::timeout(5s, exercise()));
  CHECK(support::close_socket(idle) == 0);
  REQUIRE(result);
  REQUIRE(listener->close());
  CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.empty());
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

TEST_CASE("Context shutdown drains TCP serve and its pending clients")
{
  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    auto idle = support::connect(listener->local_port());
    ServerState state;
    auto server = ctx->spawn(
      weave::tcp::serve(*listener, {}, EchoHandler{state, std::make_unique<HandlerLifetime>(state)}));
    REQUIRE(server);
    auto wait = [&]() -> weave::Task<void> {
      while (state.entered == 0)
        co_await ctx->yield();
    };
    REQUIRE(ctx->run(weave::timeout(5s, wait())));
    ctx->request_stop();
    ctx->shutdown();
    CHECK(support::close_socket(idle) == 0);
    REQUIRE(server->ready());
    auto result = std::move(*server).get();
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
    CHECK(state.active == 0);
    CHECK(state.destroyed == 1);
    CHECK(state.handler_destroyed == 1);
    REQUIRE(listener->close());
    CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.empty());
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}

TEST_CASE("Precancelling TCP serve reclaims handlers without opening sockets")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  weave::CancelSource stop;
  stop.cancel();
  ServerState state;
  int observed = 0;
  auto server = ctx->spawn(
    weave::tcp::serve(
      "127.0.0.1",
      0,
      {},
      EchoHandler{state, std::make_unique<HandlerLifetime>(state)},
      [&](weave::Error) noexcept {
        ++observed;
      }),
    {.cancel = stop.token()});
  REQUIRE(server);
  auto result = ctx->run(std::move(*server).as_task());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(state.entered == 0);
  CHECK(state.handler_destroyed == 1);
  CHECK(observed == 0);
  CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.empty());
  CHECK(ctx->metrics().submitted == 0);
}
