#include <doctest/doctest.h>
#include <weave/tcp.hpp>
#include <array>
#include <limits>
#include <type_traits>
#include <utility>

using namespace std::chrono_literals;

TEST_CASE("TCP keepalive and user-timeout configuration are synchronous")
{
  using Stream = weave::TcpStream;
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().keep_alive()), weave::Result<void>>);
  static_assert(std::is_same_v<
    decltype(std::declval<const Stream &>().keep_alive_options()),
    weave::Result<weave::TcpKeepAliveOptions>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().user_timeout(1ms)), weave::Result<void>>);
  static_assert(
    std::is_same_v<decltype(std::declval<const Stream &>().user_timeout()), weave::Result<std::chrono::milliseconds>>);
}

TEST_CASE("TCP keepalive tuning preserves unspecified values and rejects invalid input before mutation")
{
  const std::array hosts{"127.0.0.1", "::1"};
  const std::array completion_modes{false, true};
  for (auto host : hosts) {
    for (auto skip : completion_modes) {
      CAPTURE(host);
      CAPTURE(skip);
      auto ctx = weave::Context::create({.skip_successful_completions = skip});
      REQUIRE(ctx);
      if (!ctx)
        return;
      auto listener = weave::tcp::listen(*ctx, host, 0);
      REQUIRE(listener);
      if (!listener)
        return;
      auto client = ctx->run(weave::tcp::connect(*ctx, host, listener->local_port()));
      REQUIRE(client);
      if (!client)
        return;
      auto peer = ctx->run(listener->accept());
      REQUIRE(peer);
      if (!peer)
        return;

      const auto submitted = ctx->metrics().submitted;
      REQUIRE(client->keep_alive({.idle = 60s, .interval = 5s, .probes = 3}));
      auto state = client->keep_alive_options();
      REQUIRE(state);
      if (!state)
        return;
      CHECK(state->enabled);
      CHECK(state->idle == 60s);
      CHECK(state->interval == 5s);
      CHECK(state->probes == 3);

      REQUIRE(client->keep_alive({.enabled = false, .idle = 90s, .interval = 7s, .probes = 4}));
      state = client->keep_alive_options();
      REQUIRE(state);
      if (!state)
        return;
      CHECK_FALSE(state->enabled);
      CHECK(state->idle == 60s);
      CHECK(state->interval == 5s);
      CHECK(state->probes == 3);

      constexpr auto overflow = static_cast<std::chrono::seconds::rep>(std::numeric_limits<int>::max()) + 1;
      const std::array invalid_options{
        weave::TcpKeepAliveOptions{.idle = -1s},
        weave::TcpKeepAliveOptions{.interval = -1s},
        weave::TcpKeepAliveOptions{.idle = std::chrono::seconds{overflow}},
        weave::TcpKeepAliveOptions{.interval = std::chrono::seconds{overflow}},
        weave::TcpKeepAliveOptions{.enabled = false, .probes = std::numeric_limits<weave::u32>::max()}};
      for (auto options : invalid_options) {
        auto invalid = client->keep_alive(options);
        CHECK((!invalid && invalid.error() == std::errc::invalid_argument));
      }
      state = client->keep_alive_options();
      REQUIRE(state);
      if (!state)
        return;
      CHECK_FALSE(state->enabled);
      CHECK(state->idle == 60s);
      CHECK(state->interval == 5s);
      CHECK(state->probes == 3);

      REQUIRE(client->keep_alive());
      state = client->keep_alive_options();
      REQUIRE(state);
      if (!state)
        return;
      CHECK(state->enabled);
      CHECK(state->idle == 60s);
      CHECK(state->interval == 5s);
      CHECK(state->probes == 3);
      CHECK(ctx->metrics().submitted == submitted);

      auto native_error = client->keep_alive({.idle = 90s, .probes = 256});
      REQUIRE_FALSE(native_error);
      state = client->keep_alive_options();
      REQUIRE(state);
      if (!state)
        return;
      CHECK(state->enabled);
      CHECK(state->idle == 90s);
      CHECK(state->interval == 5s);
      CHECK(state->probes == 3);
      REQUIRE(client->keep_alive({.idle = 60s}));

      const std::array payload{std::byte{42}};
      std::array<std::byte, 1> received{};
      REQUIRE(ctx->run(client->write_all(payload)));
      REQUIRE(ctx->run(peer->read_exactly(received)));
      CHECK(received == payload);

      REQUIRE(client->close());
      CHECK_FALSE(client->keep_alive());
      CHECK_FALSE(client->keep_alive_options());
    }
  }
}

TEST_CASE("TCP user-timeout uses milliseconds and reports unsupported Windows semantics explicitly")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (!ctx)
    return;
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  if (!listener)
    return;
  auto client = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", listener->local_port()));
  REQUIRE(client);
  if (!client)
    return;
  auto peer = ctx->run(listener->accept());
  REQUIRE(peer);

  constexpr auto overflow = static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max()) + 1;
  const std::array invalid_values{-1ms, std::chrono::milliseconds{overflow}};
  for (auto value : invalid_values) {
    auto invalid = client->user_timeout(value);
    CHECK((!invalid && invalid.error() == std::errc::invalid_argument));
  }

#if defined(_WIN32)
  const std::array unsupported_values{0ms, 1234ms};
  for (auto value : unsupported_values) {
    auto unsupported = client->user_timeout(value);
    CHECK((!unsupported && unsupported.error() == std::errc::operation_not_supported));
  }
  auto queried = client->user_timeout();
  CHECK((!queried && queried.error() == std::errc::operation_not_supported));
#else
  REQUIRE(client->user_timeout(1234ms));
  CHECK(client->user_timeout() == 1234ms);
  REQUIRE(client->user_timeout(0ms));
  CHECK(client->user_timeout() == 0ms);
  REQUIRE(client->close());
  CHECK_FALSE(client->user_timeout(1ms));
  CHECK_FALSE(client->user_timeout());
#endif
}
