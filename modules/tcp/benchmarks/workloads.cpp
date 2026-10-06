#include <weave/tcp.hpp>
#include "common.hpp"
#include "echo_peer.hpp"
#include <asio/experimental/parallel_group.hpp>
#include <array>
#include <memory>
#include <tuple>
#include <vector>

namespace bench::workloads {

using asio::ip::tcp;
using bench::use_result;

// Use Asio's flat parallel group, not an artificial timer-based join or a
// nested tree of operator&& calls. Both implementations join all children.
template <class... Operations>
static asio::awaitable<void> asio_join(Operations... operations)
{
  auto executor = co_await asio::this_coro::executor;
  auto results = co_await asio::experimental::make_parallel_group(
    asio::co_spawn(executor, std::move(operations), asio::deferred)...)
                   .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
  std::apply(
    [](const auto &, auto... errors) {
      if ((... || static_cast<bool>(errors)))
        std::abort();
    },
    results);
}

struct Buffers {
  std::vector<std::byte> tx, rx;
  bool ok = true;

  explicit Buffers(std::size_t size) : tx(size), rx(size)
  {
    for (std::size_t i = 0; i < size; ++i)
      tx[i] = static_cast<std::byte>(i % 251);
  }
};

static weave::Task<void> weave_send(weave::TcpStream &socket, Buffers &data)
{
  if (!(co_await weave::as_result(socket.write_all(data.tx)))) {
    data.ok = false;
    // Wake the peer and the matching receiver on an error.
    (void)socket.shutdown_send();
  }
}

static weave::Task<void> weave_receive(weave::TcpStream &socket, Buffers &data)
{
  if (!(co_await weave::as_result(socket.read_exactly(data.rx))))
    data.ok = false;
}

static asio::awaitable<void> asio_send(tcp::socket &socket, Buffers &data)
{
  auto [ec, n] = co_await asio::async_write(socket, asio::buffer(data.tx), use_result);
  if (ec || n != data.tx.size()) {
    data.ok = false;
    asio::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_send, ignored);
  }
}

static asio::awaitable<void> asio_receive(tcp::socket &socket, Buffers &data)
{
  auto [ec, n] = co_await asio::async_read(socket, asio::buffer(data.rx), use_result);
  if (ec || n != data.rx.size())
    data.ok = false;
}

static weave::Task<void> weave_bulk(benchmark::State &state, weave::TcpStream &socket, Buffers &data)
{
  for (auto _ : state) {
    co_await weave::when_all(weave_send(socket, data), weave_receive(socket, data));
    if (!data.ok) {
      state.SkipWithError("Bulk I/O failed");
      co_return;
    }
    benchmark::ClobberMemory();
  }
}

static asio::awaitable<void> asio_bulk(benchmark::State &state, tcp::socket &socket, Buffers &data)
{
  for (auto _ : state) {
    co_await asio_join(asio_send(socket, data), asio_receive(socket, data));
    if (!data.ok) {
      state.SkipWithError("Bulk I/O failed");
      co_return;
    }
    benchmark::ClobberMemory();
  }
}

static void account(benchmark::State &state, weave::i64 size, weave::i64 connections)
{
  state.SetBytesProcessed(state.iterations() * size * connections * 2);
  state.SetItemsProcessed(state.iterations() * connections);
}

static void WeaveBulk(benchmark::State &state)
{
  support::EchoPeer peer;
  auto ctx = weave::Context::create();
  if (!ctx) {
    state.SkipWithError("Context failed");
    return;
  }

  auto socket = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", peer.port()));
  if (!socket || !socket->no_delay()) {
    state.SkipWithError("Connection failed");
    return;
  }
  Buffers data(static_cast<std::size_t>(state.range(0)));
  bench::Profile profile(*ctx);
  if (!ctx->run(weave_bulk(state, *socket, data)))
    state.SkipWithError("Bulk task failed");
  profile.report(state);
  if (data.tx != data.rx)
    state.SkipWithError("Payload mismatch");
  if (!socket->close())
    state.SkipWithError("Close failed");
  peer.join();
  if (!peer.ok())
    state.SkipWithError("Peer failed");
  account(state, state.range(0), 1);
}

static void AsioBulk(benchmark::State &state)
{
  support::EchoPeer peer;
  asio::io_context ctx;
  tcp::socket socket(ctx);
  asio::error_code ec;
  socket.connect(tcp::endpoint(asio::ip::address_v4::loopback(), peer.port()), ec);
  if (ec) {
    state.SkipWithError("Connect failed");
    return;
  }
  socket.set_option(tcp::no_delay(true), ec);
  if (ec) {
    state.SkipWithError("Socket configuration failed");
    return;
  }
  Buffers data(static_cast<std::size_t>(state.range(0)));
  asio::co_spawn(ctx, asio_bulk(state, socket, data), bench::completed);
  ctx.run();
  if (data.tx != data.rx)
    state.SkipWithError("Payload mismatch");
  socket.close(ec);
  if (ec)
    state.SkipWithError("Close failed");
  peer.join();
  if (!peer.ok())
    state.SkipWithError("Peer failed");
  account(state, state.range(0), 1);
}

static weave::Task<void> weave_exchange(weave::TcpStream &socket, Buffers &data)
{
  co_await weave_send(socket, data);
  if (data.ok)
    co_await weave_receive(socket, data);
}

static asio::awaitable<void> asio_exchange(tcp::socket &socket, Buffers &data)
{
  co_await asio_send(socket, data);
  if (data.ok)
    co_await asio_receive(socket, data);
}

template <std::size_t... I>
static weave::Task<void> weave_batches(
  benchmark::State &state,
  std::vector<weave::TcpStream> &sockets,
  std::vector<Buffers> &data,
  std::index_sequence<I...>)
{
  for (auto _ : state) {
    co_await weave::when_all(weave_exchange(sockets[I], data[I])...);
    if (!(... && data[I].ok)) {
      state.SkipWithError("Batch I/O failed");
      co_return;
    }
    benchmark::ClobberMemory();
  }
}

template <std::size_t... I>
static asio::awaitable<void> asio_batches(
  benchmark::State &state,
  std::vector<tcp::socket> &sockets,
  std::vector<Buffers> &data,
  std::index_sequence<I...>)
{
  for (auto _ : state) {
    co_await asio_join(asio_exchange(sockets[I], data[I])...);
    if (!(... && data[I].ok)) {
      state.SkipWithError("Batch I/O failed");
      co_return;
    }
    benchmark::ClobberMemory();
  }
}

template <std::size_t N>
static void WeaveConcurrent(benchmark::State &state)
{
  std::vector<std::unique_ptr<support::EchoPeer>> peers;
  auto ctx = weave::Context::create();
  if (!ctx) {
    state.SkipWithError("Context failed");
    return;
  }

  std::vector<weave::TcpStream> sockets;
  std::vector<Buffers> data;
  for (std::size_t i = 0; i < N; ++i) {
    peers.push_back(std::make_unique<support::EchoPeer>());
    auto socket = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", peers.back()->port()));
    if (!socket || !socket->no_delay()) {
      state.SkipWithError("Connection failed");
      return;
    }
    sockets.push_back(std::move(*socket));
    data.emplace_back(static_cast<std::size_t>(state.range(0)));
  }
  bench::Profile profile(*ctx);
  if (!ctx->run(weave_batches(state, sockets, data, std::make_index_sequence<N>{})))
    state.SkipWithError("Batch task failed");
  profile.report(state);
  for (std::size_t i = 0; i < N; ++i) {
    if (data[i].tx != data[i].rx)
      state.SkipWithError("Payload mismatch");
    if (!sockets[i].close())
      state.SkipWithError("Close failed");
    peers[i]->join();
    if (!peers[i]->ok())
      state.SkipWithError("Peer failed");
  }
  state.counters["connections"] = static_cast<weave::f64>(N);
  account(state, state.range(0), N);
}

template <std::size_t N>
static void AsioConcurrent(benchmark::State &state)
{
  std::vector<std::unique_ptr<support::EchoPeer>> peers;
  asio::io_context ctx;
  std::vector<tcp::socket> sockets;
  std::vector<Buffers> data;
  for (std::size_t i = 0; i < N; ++i) {
    peers.push_back(std::make_unique<support::EchoPeer>());
    sockets.emplace_back(ctx);
    asio::error_code ec;
    sockets.back().connect(tcp::endpoint(asio::ip::address_v4::loopback(), peers.back()->port()), ec);
    if (ec) {
      state.SkipWithError("Connection failed");
      return;
    }
    sockets.back().set_option(tcp::no_delay(true), ec);
    if (ec) {
      state.SkipWithError("Socket configuration failed");
      return;
    }
    data.emplace_back(static_cast<std::size_t>(state.range(0)));
  }
  asio::co_spawn(ctx, asio_batches(state, sockets, data, std::make_index_sequence<N>{}), bench::completed);
  ctx.run();
  for (std::size_t i = 0; i < N; ++i) {
    if (data[i].tx != data[i].rx)
      state.SkipWithError("Payload mismatch");
    asio::error_code ec;
    sockets[i].close(ec);
    if (ec)
      state.SkipWithError("Close failed");
    peers[i]->join();
    if (!peers[i]->ok())
      state.SkipWithError("Peer failed");
  }
  state.counters["connections"] = static_cast<weave::f64>(N);
  account(state, state.range(0), N);
}

BENCHMARK(WeaveBulk)->Arg(1 << 20)->Arg(8 << 20)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(AsioBulk)->Arg(1 << 20)->Arg(8 << 20)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(WeaveConcurrent, 8)->Arg(1024)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(AsioConcurrent, 8)->Arg(1024)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(WeaveConcurrent, 32)->Arg(1024)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(AsioConcurrent, 32)->Arg(1024)->UseRealTime()->Unit(benchmark::kMicrosecond);

} // namespace bench::workloads
