#include <doctest/doctest.h>
#include <weave/tcp/on_data.hpp>
#include <weave/tcp/serve.hpp>
#include <weave/timer.hpp>
#include "windows/iocp.hpp"
#include <memory>
#include <vector>
#include <algorithm>

using namespace std::chrono_literals;

static weave::Task<void> data_echo(weave::TcpStream &client, std::span<const std::byte> data)
{
  co_await client.write_all(data);
}

template <std::size_t Size>
concept DataBufferSize = requires { weave::tcp::on_data<Size>(data_echo); };

TEST_CASE("TCP on_data requires an asynchronous callback and nonempty buffer")
{
  using Adapter = decltype(weave::tcp::on_data(data_echo));
  static_assert(weave::detail::TcpServeHandler<Adapter>);
  static_assert(std::invocable<Adapter &, weave::TcpStream>);
  // An adapter's coroutine borrows its callable, so invoking a temporary is rejected.
  static_assert(!std::invocable<Adapter &&, weave::TcpStream>);
  static_assert(DataBufferSize<1>);
  static_assert(!DataBufferSize<0>);
  using Synchronous = decltype([](weave::TcpStream &, std::span<const std::byte>) {});
  using MutableBytes = decltype([](weave::TcpStream &, std::span<std::byte>) -> weave::Task<void> { co_return; });
  static_assert(!weave::detail::TcpDataCallback<Synchronous>);
  static_assert(!weave::detail::TcpDataCallback<MutableBytes>);
}

struct DataState {
  int calls = 0;
  int active = 0;
  int destroyed = 0;
  int handler_destroyed = 0;
  std::size_t bytes = 0;
  const std::byte *buffer = nullptr;
  bool check_reuse = false;
};

struct DataHandlerLifetime {
  DataState &state;

  ~DataHandlerLifetime()
  {
    CHECK(state.active == 0);
    ++state.handler_destroyed;
  }
};

struct BorrowedData {
  DataState &state;
  std::span<const std::byte> data;
  std::vector<std::byte> copy;

  BorrowedData(DataState &state, std::span<const std::byte> data)
      : state(state), data(data), copy(data.begin(), data.end())
  {
    CHECK_FALSE(data.empty());
    if (state.check_reuse) {
      CHECK(state.active == 0);
      if (state.buffer)
        CHECK(state.buffer == data.data());
      else
        state.buffer = data.data();
    }
    ++state.calls;
    ++state.active;
    state.bytes += data.size();
  }

  ~BorrowedData()
  {
    // Inspect the borrowed slice during frame destruction, including failed await chains.
    CHECK(std::equal(data.begin(), data.end(), copy.begin(), copy.end()));
    CHECK(state.handler_destroyed == 0);
    --state.active;
    ++state.destroyed;
  }
};

static weave::Task<void> data_roundtrip(weave::u16 port)
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

TEST_CASE("TCP on_data reuses its buffer only after awaited handlers finish and skips EOF")
{
  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    DataState state{.check_reuse = true};
    int errors = 0;
    auto server = ctx->spawn(
      weave::tcp::serve(
        *listener,
        {},
        weave::tcp::on_data<64>(
          [lifetime = std::make_unique<DataHandlerLifetime>(state),
            &ctx,
            &state](weave::TcpStream &client, std::span<const std::byte> data) -> weave::Task<void> {
            CHECK(lifetime != nullptr);
            CHECK(data.size() <= 64);
            BorrowedData borrowed{state, data};
            co_await ctx->yield();
            co_await client.write_all(data);
          }),
        [&](weave::Error) noexcept { ++errors; }));
    REQUIRE(server);
    CHECK(state.calls == 0);

    auto exercise = [&]() -> weave::Task<void> {
      co_await data_roundtrip(listener->local_port());
      CHECK(state.calls >= 129);
      CHECK(state.bytes == 8193);
      CHECK(state.active == 0);
      CHECK(state.destroyed == state.calls);
      auto calls = state.calls;

      auto empty = co_await weave::tcp::connect("127.0.0.1", listener->local_port());
      REQUIRE(empty.shutdown_send());
      std::array<std::byte, 1> buffer{};
      CHECK(co_await empty.read(buffer) == 0);
      CHECK(state.calls == calls);
      REQUIRE(empty.close());

      server->cancel();
      auto result = co_await weave::as_result(std::move(*server));
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::operation_canceled);
      CHECK(state.handler_destroyed == 1);
      CHECK(errors == 0);
    };
    REQUIRE(ctx->run(weave::timeout(5s, exercise())));
    REQUIRE(listener->close());
    CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.empty());
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}

TEST_CASE("TCP on_data isolates handler errors and retains borrowed data through cancellation")
{
  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    DataState state;
    int failures = 0, cancellations = 0;
    auto server = ctx->spawn(
      weave::tcp::serve(
        *listener,
        {},
        weave::tcp::on_data(
          [lifetime = std::make_unique<DataHandlerLifetime>(state),
            &ctx,
            &state](weave::TcpStream &client, std::span<const std::byte> data) -> weave::Task<void> {
            CHECK(lifetime != nullptr);
            BorrowedData borrowed{state, data};
            if (data.front() == std::byte{254}) {
              co_await weave::sleep_for(1h);
              FAIL("Cancelled data handler resumed its body");
            }
            co_await ctx->yield();
            if (data.front() == std::byte{255})
              co_await weave::fail(std::errc::io_error);
            co_await client.write_all(data);
          }),
        [&](weave::Error error) noexcept {
          if (error == std::errc::operation_canceled)
            ++cancellations;
          else {
            CHECK(error == std::errc::io_error);
            ++failures;
          }
        }));
    REQUIRE(server);
    auto exercise = [&]() -> weave::Task<void> {
      auto failing = co_await weave::tcp::connect("127.0.0.1", listener->local_port());
      std::array message{std::byte{255}};
      co_await failing.write_all(message);
      while (failures == 0)
        co_await ctx->yield();
      CHECK(state.active == 0);
      CHECK(state.destroyed == 1);
      REQUIRE(failing.close());
      co_await data_roundtrip(listener->local_port());

      auto idle = co_await weave::tcp::connect("127.0.0.1", listener->local_port());
      auto held = co_await weave::tcp::connect("127.0.0.1", listener->local_port());
      message[0] = std::byte{254};
      co_await held.write_all(message);
      while (state.active == 0 || ctx->metrics().submitted - ctx->metrics().completed < 2)
        co_await ctx->yield();
      auto destroyed = state.destroyed;
      server->cancel();
      auto result = co_await weave::as_result(std::move(*server));
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::operation_canceled);
      CHECK(state.active == 0);
      CHECK(state.destroyed == destroyed + 1);
      CHECK(state.handler_destroyed == 1);
      CHECK(failures == 1);
      CHECK(cancellations == 2);
      REQUIRE(idle.close());
      REQUIRE(held.close());
    };
    REQUIRE(ctx->run(weave::timeout(5s, exercise())));
    REQUIRE(listener->close());
    CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.empty());
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}
