#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/weave.hpp>
#include "echo_peer.hpp"
#include <atomic>
#include <vector>

weave::Async<int> value(int& calls) { ++calls; co_return 42; }
weave::Async<int> nested(int& calls) { co_return 1 + co_await value(calls); }

TEST_CASE("Async is lazy, move-only, and supports nested symmetric transfer") {
    weave::Context ctx;
    REQUIRE(ctx.status());
    int calls = 0;
    { auto unused = value(calls); }
    CHECK(calls == 0);
    auto operation = nested(calls);
    auto moved = std::move(operation);
    CHECK(ctx.block_on(std::move(moved)) == 43);
    CHECK(calls == 1);
    CHECK(ctx.metrics().submitted == 0);
}

TEST_CASE("Invalid endpoints and conflicting binds report values") {
    weave::Context ctx;
    REQUIRE(ctx.status());
    CHECK_FALSE(ctx.listen("not-an-ip", 0));
    CHECK_FALSE(ctx.block_on(ctx.connect("not-an-ip", 80)));
    auto first = ctx.listen("127.0.0.1", 0);
    REQUIRE(first);
    auto port = first->local_port();
    REQUIRE(port);
    CHECK(*port != 0);
    CHECK_FALSE(ctx.listen("127.0.0.1", *port));
    CHECK(first->close());
    CHECK(first->close());
    CHECK_FALSE(ctx.block_on(first->accept()));
}

TEST_CASE("ConnectEx refusal becomes an error completion") {
    weave::Context ctx;
    auto listener = ctx.listen("127.0.0.1", 0);
    REQUIRE(listener);
    auto port = listener->local_port();
    REQUIRE(port);
    REQUIRE(listener->close());
    CHECK_FALSE(ctx.block_on(ctx.connect("127.0.0.1", *port)));
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

TEST_CASE("TCP roundtrip preserves payload across sizes and partial receives") {
    for (auto size : {std::size_t{1}, std::size_t{4096}, std::size_t{1024 * 1024}}) {
        CAPTURE(size);
        support::EchoPeer peer(997);
        weave::Context ctx;
        auto client = ctx.block_on(ctx.connect("127.0.0.1", peer.port()));
        REQUIRE(client);
        REQUIRE(client->no_delay());
        std::vector<std::byte> sent(size), received(size);
        for (std::size_t i = 0; i < size; ++i) sent[i] = static_cast<std::byte>(i % 251);
        CHECK(ctx.block_on(client->write_all(sent)));
        CHECK(ctx.block_on(client->read_exactly(received)));
        CHECK(sent == received);
        CHECK(ctx.block_on(client->write_all({})));
        auto empty = ctx.block_on(client->read({}));
        REQUIRE(empty);
        CHECK(*empty == 0);
        REQUIRE(client->shutdown_send());
        std::array<std::byte, 1> tail{};
        auto eof = ctx.block_on(client->read(tail));
        REQUIRE(eof);
        CHECK(*eof == 0);
        CHECK_FALSE(ctx.block_on(client->read_exactly(tail)));
        CHECK(client->close());
        CHECK_FALSE(ctx.block_on(client->read(tail)));
        CHECK_FALSE(ctx.block_on(client->write_all(tail)));
        CHECK(ctx.metrics().submitted == ctx.metrics().completed);
        peer.join();
        CHECK(peer.ok());
    }
}

TEST_CASE("AcceptEx returns usable streams and RAII releases moved handles") {
    weave::Context ctx;
    auto listener = ctx.listen("127.0.0.1", 0);
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
    auto accepted = ctx.block_on(listener->accept());
    if (accepted) {
        auto stream = std::move(*accepted);
        std::array<std::byte, 4> buffer{};
        CHECK(ctx.block_on(stream.read_exactly(buffer)));
        CHECK(ctx.block_on(stream.write_all(buffer)));
    }
    peer.join();
    REQUIRE(accepted);
    CHECK(ok.load());
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

weave::Async<void> set_value(int& target) { target = 1; co_return; }
TEST_CASE("when_all handles immediate children and empty groups") {
    weave::Context ctx;
    int a = 0, b = 0;
    ctx.block_on(weave::when_all(set_value(a), set_value(b)));
    CHECK(a == 1);
    CHECK(b == 1);
    ctx.block_on(weave::when_all());
}

weave::Async<void> read_cancelled(weave::TcpStream& client, bool& cancelled) {
    std::array<std::byte, 8> buffer{};
    auto result = co_await client.read(buffer);
    cancelled = !result && result.error().value() == ERROR_OPERATION_ABORTED;
}
weave::Async<void> cancel_read(weave::TcpStream& client, bool& guarded) {
    std::array<std::byte, 1> buffer{};
    auto second = co_await client.read(buffer);
    guarded = !second && second.error() == std::errc::operation_in_progress &&
              !client.close() && static_cast<bool>(client.cancel());
}
TEST_CASE("Read cancellation drains completion before releasing the buffer") {
    support::EchoPeer peer;
    weave::Context ctx;
    auto client = ctx.block_on(ctx.connect("127.0.0.1", peer.port()));
    REQUIRE(client);
    bool cancelled = false, guarded = false;
    ctx.block_on(weave::when_all(read_cancelled(*client, cancelled), cancel_read(*client, guarded)));
    CHECK(cancelled);
    CHECK(guarded);
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
    CHECK(client->cancel()); // No outstanding operation is a successful no-op.
    CHECK(client->close());
    peer.join();
    CHECK(peer.ok());
}

weave::Async<void> accept_cancelled(weave::TcpListener& listener, bool& cancelled) {
    auto result = co_await listener.accept();
    cancelled = !result && result.error().value() == ERROR_OPERATION_ABORTED;
}
weave::Async<void> cancel_accept(weave::TcpListener& listener, bool& guarded) {
    auto second = co_await listener.accept();
    guarded = !second && second.error() == std::errc::operation_in_progress &&
              !listener.close() && static_cast<bool>(listener.cancel());
}
TEST_CASE("Accept cancellation closes the unaccepted socket after completion") {
    weave::Context ctx;
    auto listener = ctx.listen("127.0.0.1", 0);
    REQUIRE(listener);
    bool cancelled = false, guarded = false;
    ctx.block_on(weave::when_all(accept_cancelled(*listener, cancelled), cancel_accept(*listener, guarded)));
    CHECK(cancelled);
    CHECK(guarded);
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

weave::Async<void> echo_once(weave::TcpListener& listener, bool& ok) {
    auto socket = co_await listener.accept();
    if (!socket) co_return;
    std::array<std::byte, 128> buffer{};
    auto read = co_await socket->read_exactly(buffer);
    if (!read) co_return;
    ok = static_cast<bool>(co_await socket->write_all(buffer));
}
weave::Async<void> send_once(weave::Context& ctx, std::uint16_t port, bool& ok) {
    auto socket = co_await ctx.connect("127.0.0.1", port);
    if (!socket) co_return;
    std::array<std::byte, 128> tx{}, rx{};
    tx.fill(std::byte{0x42});
    auto written = co_await socket->write_all(tx);
    if (!written) co_return;
    auto read = co_await socket->read_exactly(rx);
    ok = read && rx == tx;
}
TEST_CASE("Concurrent client and server share one context and drain all completions") {
    weave::Context ctx;
    auto listener = ctx.listen("127.0.0.1", 0);
    REQUIRE(listener);
    auto port = listener->local_port();
    REQUIRE(port);
    for (int i = 0; i < 100; ++i) {
        bool server_ok = false, client_ok = false;
        ctx.block_on(weave::when_all(echo_once(*listener, server_ok), send_once(ctx, *port, client_ok)));
        CHECK(server_ok);
        CHECK(client_ok);
    }
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}

weave::Async<void> echo_many(weave::TcpListener& listener, int count, bool& ok) {
    ok = true;
    for (int i = 0; i < count; ++i) {
        bool echoed = false;
        co_await echo_once(listener, echoed);
        ok = ok && echoed;
    }
}
TEST_CASE("Many simultaneous connects and sends exercise batched completion storage") {
    weave::Context ctx;
    auto listener = ctx.listen("127.0.0.1", 0);
    REQUIRE(listener);
    auto port = listener->local_port();
    REQUIRE(port);
    bool server_ok = false;
    std::array<bool, 8> ok{};
    ctx.block_on(weave::when_all(
        echo_many(*listener, 8, server_ok),
        send_once(ctx, *port, ok[0]), send_once(ctx, *port, ok[1]),
        send_once(ctx, *port, ok[2]), send_once(ctx, *port, ok[3]),
        send_once(ctx, *port, ok[4]), send_once(ctx, *port, ok[5]),
        send_once(ctx, *port, ok[6]), send_once(ctx, *port, ok[7])));
    CHECK(server_ok);
    for (bool value : ok) CHECK(value);
    CHECK(ctx.metrics().submitted == ctx.metrics().completed);
}
