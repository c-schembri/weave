#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/tcp.hpp>
#include <weave/timer.hpp>
#include "echo_peer.hpp"
#include "windows/iocp.hpp"
#include <atomic>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

TEST_CASE("Synchronous socket operations return Results rather than Tasks")
{
  using Stream = weave::TcpStream;
  using Listener = weave::TcpListener;
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().no_delay()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().shutdown_send()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().cancel()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Stream &>().close()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Listener &>().cancel()), weave::Result<void>>);
  static_assert(std::is_same_v<decltype(std::declval<Listener &>().close()), weave::Result<void>>);
  static_assert(std::is_same_v<
    decltype(weave::tcp::listen(std::declval<weave::Context &>(), "127.0.0.1", 0)),
    weave::Result<Listener>>);
}

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
    auto ctx = weave::Context::create();
    REQUIRE(ctx);

    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    REQUIRE(listener->local_port() != 0);
    REQUIRE(listener->close());

    int calls = 0;
    CHECK(ctx->run(value(calls)) == 42);
    CHECK(calls == 1);
    CHECK(weave::detail::current_context == nullptr);
  }

  CHECK(weave::detail::current_context == nullptr);
}

TEST_CASE("Listeners cache assigned ports across moves and close")
{
  static_assert(std::is_same_v<decltype(std::declval<const weave::TcpListener &>().local_port()), weave::u16>);
  static_assert(noexcept(std::declval<const weave::TcpListener &>().local_port()));

  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    auto port = listener->local_port();
    REQUIRE(port != 0);
    CHECK(listener->local_port() == port);

    auto moved = std::move(*listener);
    CHECK(moved.local_port() == port);
    CHECK(listener->local_port() == 0);
    REQUIRE(moved.close());
    CHECK(moved.local_port() == port);

    auto rebound = weave::tcp::listen(*ctx, "127.0.0.1", port);
    REQUIRE(rebound);
    CHECK(rebound->local_port() == port);
  }
}

TEST_CASE("Task is lazy, move-only, and supports nested symmetric transfer")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  int calls = 0;
  {
    auto unused = value(calls);
  }
  CHECK(calls == 0);
  auto operation = nested(calls);
  auto moved = std::move(operation);
  CHECK(ctx->run(std::move(moved)) == 43);
  CHECK(calls == 1);
  CHECK(ctx->metrics().submitted == 0);
}

TEST_CASE("Invalid endpoints and conflicting binds report values")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  CHECK_FALSE(weave::tcp::listen(*ctx, "not-an-ip", 0));
  CHECK_FALSE(ctx->run(weave::tcp::connect(*ctx, "bad host", 80)));
  auto first = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(first);
  auto port = first->local_port();
  REQUIRE(port != 0);
  CHECK_FALSE(weave::tcp::listen(*ctx, "127.0.0.1", port));
  CHECK(first->close());
  CHECK(first->close());
  CHECK_FALSE(ctx->run(first->accept()));
}

TEST_CASE("ConnectEx refusal becomes an error completion")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port != 0);
  REQUIRE(listener->close());
  CHECK_FALSE(ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", port)));
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

TEST_CASE("Explicit listen backlogs retain more than 200 unaccepted connections")
{
  constexpr unsigned count = 512;
  for (int backlog : {512, 8192}) {
    for (bool skip : {false, true}) {
      CAPTURE(backlog);
      CAPTURE(skip);
      auto ctx = weave::Context::create({.skip_successful_completions = skip});
      REQUIRE(ctx);
      auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0, backlog);
      REQUIRE(listener);

      auto queue = [&](weave::TaskScope &pending) -> weave::Task<void> {
        std::vector<weave::JoinHandle<weave::TcpStream>> connections;
        connections.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
          auto connection = pending.spawn(weave::tcp::connect(*ctx, "127.0.0.1", listener->local_port()));
          if (!connection)
            co_await weave::fail(connection.error());
          connections.push_back(std::move(*connection));
        }

        // Keep every client open, without accepting: retries cannot empty the listener queue.
        std::vector<weave::TcpStream> clients;
        clients.reserve(count);
        for (auto &connection : connections)
          clients.push_back(co_await std::move(connection));
        CHECK(clients.size() == count);
      };
      auto result = ctx->run(weave::timeout(std::chrono::seconds(5), weave::scope(queue)));
      CAPTURE(result ? 0 : result.error().value());
      REQUIRE(result);
      CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    }
  }
}

TEST_CASE("TCP roundtrip preserves payload across sizes and partial receives")
{
  for (auto size : {std::size_t{1}, std::size_t{4096}, std::size_t{1024 * 1024}}) {
    CAPTURE(size);
    support::EchoPeer peer(997);
    auto ctx = weave::Context::create();
    REQUIRE(ctx);

    auto client = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", peer.port()));
    REQUIRE(client);
    REQUIRE(client->no_delay());
    std::vector<std::byte> sent(size), received(size);
    for (std::size_t i = 0; i < size; ++i)
      sent[i] = static_cast<std::byte>(i % 251);
    CHECK(ctx->run(client->write_all(sent)));
    CHECK(ctx->run(client->read_exactly(received)));
    CHECK(sent == received);
    CHECK(ctx->run(client->write_all({})));
    auto empty = ctx->run(client->read({}));
    REQUIRE(empty);
    CHECK(*empty == 0);
    REQUIRE(client->shutdown_send());
    std::array<std::byte, 1> tail{};
    auto eof = ctx->run(client->read(tail));
    REQUIRE(eof);
    CHECK(*eof == 0);
    CHECK_FALSE(ctx->run(client->read_exactly(tail)));
    CHECK(client->close());
    CHECK_FALSE(ctx->run(client->read(tail)));
    CHECK_FALSE(ctx->run(client->write_all(tail)));
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    peer.join();
    CHECK(peer.ok());
  }
}

TEST_CASE("AcceptEx returns usable streams and RAII releases moved handles")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port != 0);
  std::atomic<bool> ok = false;
  std::thread peer([&] {
    SOCKET socket = support::connect(port);
    std::array<char, 4> message{'p', 'i', 'n', 'g'}, received{};
    ok = support::write_all(socket, message.data(), message.size()) &&
      support::read_exactly(socket, received.data(), received.size()) && message == received;
    closesocket(socket);
  });
  auto accepted = ctx->run(listener->accept({.no_delay = true}));
  if (accepted) {
    auto stream = std::move(*accepted);
    std::array<std::byte, 4> buffer{};
    CHECK(ctx->run(stream.read_exactly(buffer)));
    CHECK(ctx->run(stream.write_all(buffer)));
  }
  peer.join();
  REQUIRE(accepted);
  CHECK(ok.load());
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

TEST_CASE("Accept options configure the native socket before returning the stream")
{
  static_assert(std::is_same_v<
    decltype(std::declval<weave::TcpListener &>().accept({.no_delay = true})),
    weave::Task<weave::TcpStream>>);

  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    auto &io = weave::detail::IoAccess::state(*ctx);
    REQUIRE(io.registered_handles_.size() == 1);
    {
      auto unused = listener->accept({.no_delay = true});
      CHECK(io.registered_handles_.size() == 1);
    }

    // Inspect the backend registry without adding native-handle access to the public API.
    for (int mode : {0, 1, 2}) {
      CAPTURE(skip);
      CAPTURE(mode);
      auto peer = support::connect(listener->local_port());
      auto operation = mode == 0 ? listener->accept() : listener->accept({.no_delay = mode == 2});
      auto client = ctx->run(std::move(operation));
      CHECK(closesocket(peer) == 0);
      REQUIRE(client);
      REQUIRE(io.registered_handles_.size() == 2);

      BOOL enabled = FALSE;
      int size = sizeof(enabled);
      auto queried = getsockopt(
        io.registered_handles_.back(),
        IPPROTO_TCP,
        TCP_NODELAY,
        reinterpret_cast<char *>(&enabled),
        &size);
      REQUIRE(queried == 0);
      CHECK(static_cast<bool>(enabled) == (mode == 2));
      REQUIRE(client->close());
      CHECK(io.registered_handles_.size() == 1);
      CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    }

    REQUIRE(listener->close());
    auto invalid = ctx->run(listener->accept({.no_delay = true}));
    CHECK_FALSE(invalid);
    CHECK(io.registered_handles_.empty());
  }
}

static weave::Task<void> set_value(int &target)
{
  target = 1;
  co_return;
}

TEST_CASE("when_all handles immediate children and empty groups")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  int a = 0, b = 0;
  CHECK(ctx->run(weave::when_all(set_value(a), set_value(b))));
  CHECK(a == 1);
  CHECK(b == 1);
  CHECK(ctx->run(weave::when_all()));
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
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto client = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", peer.port()));
  REQUIRE(client);
  bool cancelled = false, guarded = false;
  CHECK(ctx->run(weave::when_all(read_cancelled(*client, cancelled), cancel_read(*client, guarded))));
  CHECK(cancelled);
  CHECK(guarded);
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  CHECK(client->cancel()); // No outstanding operation is a successful no-op.
  CHECK(client->close());
  peer.join();
  CHECK(peer.ok());
}

static weave::Task<void> accept_cancelled(
  weave::TcpListener &listener,
  bool &cancelled,
  weave::AcceptOptions options = {})
{
  auto result = co_await weave::as_result(listener.accept(options));
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
  for (bool skip : {false, true}) {
    for (bool no_delay : {false, true}) {
      CAPTURE(skip);
      CAPTURE(no_delay);
      auto ctx = weave::Context::create({.skip_successful_completions = skip});
      REQUIRE(ctx);

      auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
      REQUIRE(listener);
      bool cancelled = false, guarded = false;
      CHECK(ctx->run(
        weave::when_all(
          accept_cancelled(*listener, cancelled, {.no_delay = no_delay}),
          cancel_accept(*listener, guarded))));
      CHECK(cancelled);
      CHECK(guarded);
      CHECK(ctx->metrics().submitted == ctx->metrics().completed);
      CHECK(weave::detail::IoAccess::state(*ctx).registered_handles_.size() == 1);
    }
  }
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

static weave::Task<void> implicit_roundtrip(weave::Task<weave::TcpListener> setup, bool &server_ok, bool &client_ok)
{
  auto listener = co_await std::move(setup);
  auto client = [&]() -> weave::Task<void> {
    auto socket = co_await weave::tcp::connect("127.0.0.1", listener.local_port());
    std::array<std::byte, 128> tx{}, rx{};
    tx.fill(std::byte{0x42});
    co_await socket.write_all(tx);
    co_await socket.read_exactly(rx);
    client_ok = tx == rx;
  };
  co_await weave::when_all(echo_once(listener, server_ok), client());
}

TEST_CASE("Implicit TCP setup is lazy and works on a standalone Context")
{
  static_assert(std::is_same_v<decltype(weave::tcp::listen("127.0.0.1", 0)), weave::Task<weave::TcpListener>>);
  static_assert(std::is_same_v<decltype(weave::tcp::connect("127.0.0.1", 80)), weave::Task<weave::TcpStream>>);
  REQUIRE(weave::detail::current_context == nullptr);
  {
    auto unused_listener = weave::tcp::listen("127.0.0.1", 0);
    auto unused_client = weave::tcp::connect("127.0.0.1", 80);
  }

  for (bool skip : {false, true}) {
    auto setup = weave::tcp::listen("127.0.0.1", 0, 512);
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    bool server_ok = false, client_ok = false;
    REQUIRE(ctx->run(implicit_roundtrip(std::move(setup), server_ok, client_ok)));
    CHECK(server_ok);
    CHECK(client_ok);
    CHECK(ctx->metrics().submitted > 0);
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    CHECK(weave::detail::current_context == nullptr);

    auto invalid_listener = ctx->run(weave::tcp::listen("not-an-ip", 0));
    REQUIRE_FALSE(invalid_listener);
    CHECK(invalid_listener.error() == std::errc::invalid_argument);
    auto invalid_client = ctx->run(weave::tcp::connect(nullptr, 80));
    REQUIRE_FALSE(invalid_client);
    CHECK(invalid_client.error() == std::errc::invalid_argument);

    ctx->request_stop();
    auto stopped_listener = ctx->run(weave::tcp::listen("127.0.0.1", 0));
    REQUIRE_FALSE(stopped_listener);
    CHECK(stopped_listener.error() == std::errc::operation_canceled);
    auto stopped_client = ctx->run(weave::tcp::connect("127.0.0.1", 80));
    REQUIRE_FALSE(stopped_client);
    CHECK(stopped_client.error() == std::errc::operation_canceled);
  }
}

static weave::Task<weave::Task<weave::TcpListener>> defer_listener()
{
  co_return weave::tcp::listen("127.0.0.1", 0);
}

static weave::Task<weave::Task<weave::TcpStream>> defer_connect(weave::u16 port)
{
  co_return weave::tcp::connect("127.0.0.1", port);
}

TEST_CASE("Implicit TCP setup resolves the executing Context rather than the constructing Context")
{
  support::EchoPeer peer;
  auto first = weave::Context::create();
  auto second = weave::Context::create();
  REQUIRE(first);
  REQUIRE(second);
  auto listening = first->run(defer_listener());
  auto connecting = first->run(defer_connect(peer.port()));
  REQUIRE(listening);
  REQUIRE(connecting);
  first->shutdown();

  auto listener = second->run(std::move(*listening));
  REQUIRE(listener);
  CHECK(listener->local_port() != 0);
  REQUIRE(listener->close());
  auto client = second->run(std::move(*connecting));
  REQUIRE(client);
  REQUIRE(client->close());
  CHECK(first->metrics().submitted == 0);
  CHECK(second->metrics().submitted > 0);
  CHECK(second->metrics().submitted == second->metrics().completed);
  CHECK(weave::detail::current_context == nullptr);
  peer.join();
  CHECK(peer.ok());
}

TEST_CASE("Concurrent client and server share one context and drain all completions")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port != 0);
  for (int i = 0; i < 100; ++i) {
    bool server_ok = false, client_ok = false;
    CHECK(ctx->run(weave::when_all(echo_once(*listener, server_ok), send_once(*ctx, port, client_ok))));
    CHECK(server_ok);
    CHECK(client_ok);
  }
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
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
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port != 0);
  bool server_ok = false;
  std::array<bool, 8> ok{};
  CHECK(ctx->run(
    weave::when_all(
      echo_many(*listener, 8, server_ok),
      send_once(*ctx, port, ok[0]),
      send_once(*ctx, port, ok[1]),
      send_once(*ctx, port, ok[2]),
      send_once(*ctx, port, ok[3]),
      send_once(*ctx, port, ok[4]),
      send_once(*ctx, port, ok[5]),
      send_once(*ctx, port, ok[6]),
      send_once(*ctx, port, ok[7]))));
  CHECK(server_ok);
  for (bool value : ok)
    CHECK(value);
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

TEST_CASE("Both queued-success and skip-success modes complete exactly once")
{
  for (bool skip : {false, true}) {
    CAPTURE(skip);
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);

    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    auto port = listener->local_port();
    REQUIRE(port != 0);
    for (int i = 0; i < 200; ++i) {
      bool server_ok = false, client_ok = false;
      CHECK(ctx->run(weave::when_all(echo_once(*listener, server_ok), send_once(*ctx, port, client_ok))));
      REQUIRE(server_ok);
      REQUIRE(client_ok);
    }
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    if (!skip)
      CHECK(ctx->metrics().inline_completions == 0);
    bool cancelled = false, guarded = false;
    CHECK(ctx->run(weave::when_all(accept_cancelled(*listener, cancelled), cancel_accept(*listener, guarded))));
    CHECK(cancelled);
    CHECK(guarded);
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}

TEST_CASE("Large concurrent transfer drains synthetic fairness and kernel completions")
{
  for (bool skip : {false, true}) {
    CAPTURE(skip);
    support::EchoPeer peer;
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);

    auto socket = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", peer.port()));
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
    CHECK(ctx->run(weave::when_all(writer(), reader())));
    CHECK(sent);
    CHECK(received);
    CHECK(tx == rx);
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    CHECK(socket->close());
    peer.join();
    CHECK(peer.ok());
  }
}

TEST_CASE("Context stop cancels sockets registered before run and drains pending accept")
{
  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    bool started = false;
    int destroyed = 0;
    auto job = ctx->spawn([&](weave::Context &) -> weave::Task<void> {
      struct Guard {
        int &destroyed;
        ~Guard()
        {
          ++destroyed;
        }
      } guard{destroyed};
      started = true;
      (void)co_await listener->accept();
      FAIL("Cancelled accept resumed its body");
    });
    REQUIRE(job);
    auto wait = [&]() -> weave::Task<void> {
      while (!started)
        co_await ctx->yield();
    };
    REQUIRE(ctx->run(wait()));
    CHECK(ctx->metrics().submitted > ctx->metrics().completed);
    CHECK(destroyed == 0);

    std::thread stopper([&] { ctx->request_stop(); });
    stopper.join();
    ctx->shutdown();
    REQUIRE(job->ready());
    auto result = std::move(*job).get();
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
    CHECK(destroyed == 1);
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    CHECK(listener->close());
    CHECK_FALSE(weave::tcp::listen(*ctx, "127.0.0.1", 0));
  }
}

TEST_CASE("Context destruction drains a detached pending read before reclaiming its socket and frame")
{
  for (bool skip : {false, true}) {
    support::EchoPeer peer;
    std::optional<weave::JoinHandle<void>> saved;
    int destroyed = 0;
    bool started = false;
    {
      auto ctx = weave::Context::create({.skip_successful_completions = skip});
      REQUIRE(ctx);
      auto client = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", peer.port()));
      REQUIRE(client);
      auto job = ctx->spawn(
        [socket = std::move(*client), &started, &destroyed](weave::Context &) mutable -> weave::Task<void> {
          struct Guard {
            int &destroyed;
            ~Guard()
            {
              ++destroyed;
            }
          } guard{destroyed};
          std::array<std::byte, 4096> buffer;
          started = true;
          (void)co_await socket.read(buffer);
          FAIL("Cancelled read resumed its body");
        });
      REQUIRE(job);
      saved.emplace(std::move(*job));
      auto wait = [&]() -> weave::Task<void> {
        while (!started)
          co_await ctx->yield();
      };
      REQUIRE(ctx->run(wait()));
      CHECK(ctx->metrics().submitted > ctx->metrics().completed);
      CHECK(destroyed == 0);
    }
    CHECK(destroyed == 1);
    REQUIRE(saved->ready());
    auto result = std::move(*saved).get();
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
    peer.join();
    CHECK(peer.ok());
  }
}

TEST_CASE("Task error observers see native cancellation once after pending IO completes")
{
  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    int observed = 0;
    bool continued = false;
    auto accept = [&]() -> weave::Task<void> {
      (void)co_await listener->accept().on_error([&](weave::Error error) noexcept {
        CHECK(error.value() == ERROR_OPERATION_ABORTED);
        ++observed;
      });
      continued = true;
    };
    auto cancel = [&]() -> weave::Task<void> {
      co_await ctx->yield();
      if (auto status = listener->cancel(); !status)
        co_await weave::fail(status.error());
    };
    auto result = ctx->run(weave::when_all(accept(), cancel()));
    REQUIRE_FALSE(result);
    CHECK(result.error().value() == ERROR_OPERATION_ABORTED);
    CHECK(observed == 1);
    CHECK_FALSE(continued);
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}

TEST_CASE("Context shutdown drains a detached native accept and reports its error once")
{
  for (bool skip : {false, true}) {
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
    REQUIRE(listener);
    int observed = 0;
    bool started = false;
    ctx->detach(
      [&](weave::Context &) {
        started = true;
        return listener->accept();
      },
      [&](weave::Error error) noexcept {
        CHECK(error == std::errc::operation_canceled);
        ++observed;
      });
    auto wait = [&]() -> weave::Task<void> {
      while (!started)
        co_await ctx->yield();
    };
    REQUIRE(ctx->run(wait()));
    CHECK(ctx->metrics().submitted > ctx->metrics().completed);
    CHECK(observed == 0);
    ctx->shutdown();
    CHECK(observed == 1);
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}
