#include <weave/weave.hpp>
#include <asio.hpp>
#include <benchmark/benchmark.h>
#include "echo_peer.hpp"
#include <cstdio>
#include <vector>

namespace asio::detail {
template<class Exception> void throw_exception(const Exception&) {
    std::fputs("Fatal Asio failure (exceptions disabled)\n", stderr);
    std::abort();
}
}

namespace {
constexpr auto use_result = asio::as_tuple(asio::use_awaitable);
using asio::ip::tcp;

weave::Async<void> weave_roundtrips(benchmark::State& state, weave::TcpStream& socket,
                                   std::vector<std::byte>& tx, std::vector<std::byte>& rx) {
    for (auto _ : state) {
        auto sent = co_await socket.write_all(tx);
        if (!sent) { state.SkipWithError("Weave write failed"); co_return; }
        auto read = co_await socket.read_exactly(rx);
        if (!read) { state.SkipWithError("Weave read failed"); co_return; }
        benchmark::ClobberMemory();
    }
}
asio::awaitable<void> asio_roundtrips(benchmark::State& state, tcp::socket& socket,
                                    std::vector<std::byte>& tx, std::vector<std::byte>& rx) {
    for (auto _ : state) {
        auto [send_error, sent] = co_await asio::async_write(socket, asio::buffer(tx), use_result);
        if (send_error) { state.SkipWithError("Asio write failed"); co_return; }
        auto [read_error, read] = co_await asio::async_read(socket, asio::buffer(rx), use_result);
        if (read_error) { state.SkipWithError("Asio read failed"); co_return; }
        benchmark::ClobberMemory();
    }
}
void finish(benchmark::State& state, const std::vector<std::byte>& tx, const std::vector<std::byte>& rx) {
    if (tx != rx) state.SkipWithError("Payload mismatch");
    state.SetBytesProcessed(state.iterations() * state.range(0) * 2);
    state.SetItemsProcessed(state.iterations());
}
void Weave(benchmark::State& state) {
    support::EchoPeer peer;
    weave::Context ctx;
    if (!ctx.status()) { state.SkipWithError("Weave context failed"); return; }
    auto socket = ctx.block_on(ctx.connect("127.0.0.1", peer.port()));
    if (!socket || !socket->no_delay()) { state.SkipWithError("Weave setup failed"); return; }
    std::vector<std::byte> tx(static_cast<std::size_t>(state.range(0)), std::byte{0x5a}), rx(tx.size());
    const auto dequeues_before = ctx.metrics().dequeue_calls;
    ctx.block_on(weave_roundtrips(state, *socket, tx, rx));
    finish(state, tx, rx);
    if (!socket->close()) state.SkipWithError("Weave close failed");
    peer.join();
    if (!peer.ok()) state.SkipWithError("Peer failed");
    if (state.iterations() != 0)
        state.counters["dequeues/roundtrip"] =
            static_cast<double>(ctx.metrics().dequeue_calls - dequeues_before) /
            static_cast<double>(state.iterations());
}
void Asio(benchmark::State& state) {
    support::EchoPeer peer;
    asio::io_context io;
    tcp::socket socket(io);
    asio::error_code ec;
    socket.connect(tcp::endpoint(asio::ip::address_v4::loopback(), peer.port()), ec);
    if (ec) { state.SkipWithError("Asio connect failed"); return; }
    socket.set_option(tcp::no_delay(true), ec);
    if (ec) { state.SkipWithError("Asio setup failed"); return; }
    std::vector<std::byte> tx(static_cast<std::size_t>(state.range(0)), std::byte{0x5a}), rx(tx.size());
    asio::co_spawn(io, asio_roundtrips(state, socket, tx, rx), [](std::exception_ptr failure) {
        if (failure) std::abort();
    });
    io.run();
    finish(state, tx, rx);
    socket.close(ec);
    peer.join();
    if (!peer.ok()) state.SkipWithError("Peer failed");
}
BENCHMARK(Weave)->Arg(64)->Arg(1024)->Arg(65536)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(Asio)->Arg(64)->Arg(1024)->Arg(65536)->UseRealTime()->Unit(benchmark::kMicrosecond);
}
BENCHMARK_MAIN();
