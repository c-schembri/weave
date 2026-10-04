#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/tcp.hpp>
#include "echo_peer.hpp"
#include <atomic>
#include <vector>

static weave::Task<int> value(int &calls)
{
  ++calls;
  co_return 42;
}

static weave::Task<int> nested(int &calls)
{
  co_return 1 + co_await value(calls);
}

TEST_CASE("Standalone context operations do not require a current runtime context")
{
  REQUIRE(weave::detail::current_context == nullptr);

  {
    weave::Context ctx;
    REQUIRE(ctx.status());

    auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    REQUIRE(listener->local_port());
    REQUIRE(listener->close());

    int calls = 0;
    CHECK(ctx.run(value(calls)) == 42);
    CHECK(calls == 1);
    CHECK(weave::detail::current_context == nullptr);
  }

  CHECK(weave::detail::current_context == nullptr);
}

TEST_CASE("Task is lazy, move-only, and supports nested symmetric transfer")
{
  weave::Context ctx;
  REQUIRE(ctx.status());
  int calls = 0;
  {
    auto unused = value(calls);
  }
  CHECK(calls == 0);
  auto operation = nested(calls);
  auto moved = std::move(operation);
  CHECK(ctx.run(std::move(moved)) == 43);
  CHECK(calls == 1);
  CHECK(ctx.metrics().submitted == 0);
}

TEST_CASE("Invalid endpoints and conflicting binds report values")
{
  weave::Context ctx;
  REQUIRE(ctx.status());
  CHECK_FALSE(weave::tcp::listen(ctx, "not-an-ip", 0));
  CHECK_FALSE(ctx.run(weave::tcp::connect(ctx, "not-an-ip", 80)));
  auto first = weave::tcp::listen(ctx, "127.0.0.1", 0);
  REQUIRE(first);
  auto port = first->local_port();
  REQUIRE(port);
  CHECK(*port != 0);
  CHECK_FALSE(weave::tcp::listen(ctx, "127.0.0.1", *port));
  CHECK(first->close());
  CHECK(first->close());
  CHECK_FALSE(ctx.run(first->accept()));
}

TEST_CASE("ConnectEx refusal becomes an error completion")
{
  weave::Context ctx;
  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port);
  REQUIRE(listener->close());
  CHECK_FALSE(ctx.run(weave::tcp::connect(ctx, "127.0.0.1", *port)));
  CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

TEST_CASE("TCP roundtrip preserves payload across sizes and partial receives")
{
  for (auto size : {std::size_t{1}, std::size_t{4096}, std::size_t{1024 * 1024}}) {
    CAPTURE(size);
    support::EchoPeer peer(997);
    weave::Context ctx;
    auto client = ctx.run(weave::tcp::connect(ctx, "127.0.0.1", peer.port()));
    REQUIRE(client);
    REQUIRE(client->no_delay());
    std::vector<std::byte> sent(size), received(size);
    for (std::size_t i = 0; i < size; ++i)
      sent[i] = static_cast<std::byte>(i % 251);
    CHECK(ctx.run(client->write_all(sent)));
    CHECK(ctx.run(client->read_exactly(received)));
    CHECK(sent == received);
    CHECK(ctx.run(client->write_all({})));
    auto empty = ctx.run(client->read({}));
    REQUIRE(empty);
    CHECK(*empty == 0);
    REQUIRE(client->shutdown_send());
    std::array<std::byte, 1> tail{};
    auto eof = ctx.run(client->read(tail));
    REQUIRE(eof);
    CHECK(*eof == 0);
    CHECK_FALSE(ctx.run(client->read_exactly(tail)));
    CHECK(client->close());
    CHECK_FALSE(ctx.run(client->read(tail)));
    CHECK_FALSE(ctx.run(client->write_all(tail)));
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
    peer.join();
    CHECK(peer.ok());
  }
}

TEST_CASE("AcceptEx returns usable streams and RAII releases moved handles")
{
  weave::Context ctx;
  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port);
  std::atomic<bool> ok = false;
  std::thread peer([&] {
    SOCKET socket = support::connect(*port);
    std::array<char, 4> message{'p', 'i', 'n', 'g'}, received{};
    ok = support::write_all(socket, message.data(), message.size()) &&
      support::read_exactly(socket, received.data(), received.size()) && message == received;
    closesocket(socket);
  });
  auto accepted = ctx.run(listener->accept());
  if (accepted) {
    auto stream = std::move(*accepted);
    std::array<std::byte, 4> buffer{};
    CHECK(ctx.run(stream.read_exactly(buffer)));
    CHECK(ctx.run(stream.write_all(buffer)));
  }
  peer.join();
  REQUIRE(accepted);
  CHECK(ok.load());
  CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

static weave::Task<void> set_value(int &target)
{
  target = 1;
  co_return;
}

TEST_CASE("when_all handles immediate children and empty groups")
{
  weave::Context ctx;
  int a = 0, b = 0;
  CHECK(ctx.run(weave::when_all(set_value(a), set_value(b))));
  CHECK(a == 1);
  CHECK(b == 1);
  CHECK(ctx.run(weave::when_all()));
}

static weave::Task<void> read_cancelled(weave::TcpStream &client, bool &cancelled)
{
  std::array<std::byte, 8> buffer{};
  auto result = co_await weave::as_result(client.read(buffer));
  cancelled = !result && result.error().value() == ERROR_OPERATION_ABORTED;
}

static weave::Task<void> cancel_read(weave::TcpStream &client, bool &guarded)
{
  std::array<std::byte, 1> buffer{};
  auto second = co_await weave::as_result(client.read(buffer));
  guarded = !second && second.error() == std::errc::operation_in_progress && !client.close() &&
    static_cast<bool>(client.cancel());
}

TEST_CASE("Read cancellation drains completion before releasing the buffer")
{
  support::EchoPeer peer;
  weave::Context ctx;
  auto client = ctx.run(weave::tcp::connect(ctx, "127.0.0.1", peer.port()));
  REQUIRE(client);
  bool cancelled = false, guarded = false;
  CHECK(ctx.run(weave::when_all(read_cancelled(*client, cancelled), cancel_read(*client, guarded))));
  CHECK(cancelled);
  CHECK(guarded);
  CHECK(ctx.metrics().submitted == ctx.metrics().completed);
  CHECK(client->cancel()); // No outstanding operation is a successful no-op.
  CHECK(client->close());
  peer.join();
  CHECK(peer.ok());
}

static weave::Task<void> accept_cancelled(weave::TcpListener &listener, bool &cancelled)
{
  auto result = co_await weave::as_result(listener.accept());
  cancelled = !result && result.error().value() == ERROR_OPERATION_ABORTED;
}

static weave::Task<void> cancel_accept(weave::TcpListener &listener, bool &guarded)
{
  auto second = co_await weave::as_result(listener.accept());
  guarded = !second && second.error() == std::errc::operation_in_progress && !listener.close() &&
    static_cast<bool>(listener.cancel());
}

TEST_CASE("Accept cancellation closes the unaccepted socket after completion")
{
  weave::Context ctx;
  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  bool cancelled = false, guarded = false;
  CHECK(ctx.run(weave::when_all(accept_cancelled(*listener, cancelled), cancel_accept(*listener, guarded))));
  CHECK(cancelled);
  CHECK(guarded);
  CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

static weave::Task<void> echo_once(weave::TcpListener &listener, bool &ok)
{
  auto socket = co_await weave::as_result(listener.accept());
  if (!socket)
    co_return;
  std::array<std::byte, 128> buffer{};
  auto read = co_await weave::as_result(socket->read_exactly(buffer));
  if (!read)
    co_return;
  ok = static_cast<bool>(co_await weave::as_result(socket->write_all(buffer)));
}

static weave::Task<void> send_once(weave::Context &ctx, weave::u16 port, bool &ok)
{
  auto socket = co_await weave::as_result(weave::tcp::connect(ctx, "127.0.0.1", port));
  if (!socket)
    co_return;
  std::array<std::byte, 128> tx{}, rx{};
  tx.fill(std::byte{0x42});
  auto written = co_await weave::as_result(socket->write_all(tx));
  if (!written)
    co_return;
  auto read = co_await weave::as_result(socket->read_exactly(rx));
  ok = read && rx == tx;
}

TEST_CASE("Concurrent client and server share one context and drain all completions")
{
  weave::Context ctx;
  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port);
  for (int i = 0; i < 100; ++i) {
    bool server_ok = false, client_ok = false;
    CHECK(ctx.run(weave::when_all(echo_once(*listener, server_ok), send_once(ctx, *port, client_ok))));
    CHECK(server_ok);
    CHECK(client_ok);
  }
  CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

static weave::Task<void> echo_many(weave::TcpListener &listener, int count, bool &ok)
{
  ok = true;
  for (int i = 0; i < count; ++i) {
    bool echoed = false;
    co_await echo_once(listener, echoed);
    ok = ok && echoed;
  }
}

TEST_CASE("Many simultaneous connects and sends exercise batched completion storage")
{
  weave::Context ctx;
  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port);
  bool server_ok = false;
  std::array<bool, 8> ok{};
  CHECK(ctx.run(
    weave::when_all(
      echo_many(*listener, 8, server_ok),
      send_once(ctx, *port, ok[0]),
      send_once(ctx, *port, ok[1]),
      send_once(ctx, *port, ok[2]),
      send_once(ctx, *port, ok[3]),
      send_once(ctx, *port, ok[4]),
      send_once(ctx, *port, ok[5]),
      send_once(ctx, *port, ok[6]),
      send_once(ctx, *port, ok[7]))));
  CHECK(server_ok);
  for (bool value : ok)
    CHECK(value);
  CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

TEST_CASE("Both queued-success and skip-success modes complete exactly once")
{
  for (bool skip : {false, true}) {
    CAPTURE(skip);
    weave::Context ctx({.skip_successful_completions = skip});
    auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    auto port = listener->local_port();
    REQUIRE(port);
    for (int i = 0; i < 200; ++i) {
      bool server_ok = false, client_ok = false;
      CHECK(ctx.run(weave::when_all(echo_once(*listener, server_ok), send_once(ctx, *port, client_ok))));
      REQUIRE(server_ok);
      REQUIRE(client_ok);
    }
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
    if (!skip)
      CHECK(ctx.metrics().inline_completions == 0);
    bool cancelled = false, guarded = false;
    CHECK(ctx.run(weave::when_all(accept_cancelled(*listener, cancelled), cancel_accept(*listener, guarded))));
    CHECK(cancelled);
    CHECK(guarded);
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
  }
}

TEST_CASE("Large concurrent transfer drains synthetic fairness and kernel completions")
{
  for (bool skip : {false, true}) {
    CAPTURE(skip);
    support::EchoPeer peer;
    weave::Context ctx({.skip_successful_completions = skip});
    auto socket = ctx.run(weave::tcp::connect(ctx, "127.0.0.1", peer.port()));
    REQUIRE(socket);
    std::vector<std::byte> tx(8 * 1024 * 1024, std::byte{0x37}), rx(tx.size());
    bool sent = false, received = false;
    // Named coroutine lambdas remain alive until the joined work is finished.
    auto writer = [&]() -> weave::Task<void> {
      sent = static_cast<bool>(co_await weave::as_result(socket->write_all(tx)));
    };
    auto reader = [&]() -> weave::Task<void> {
      received = static_cast<bool>(co_await weave::as_result(socket->read_exactly(rx)));
    };
    CHECK(ctx.run(weave::when_all(writer(), reader())));
    CHECK(sent);
    CHECK(received);
    CHECK(tx == rx);
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
    CHECK(socket->close());
    peer.join();
    CHECK(peer.ok());
  }
}
