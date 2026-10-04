#include <weave/tcp.hpp>
#include "common.hpp"
#include "asio_pool.hpp"
#include "async_echo_peer.hpp"
#include <weave/runtime.hpp>
#include <future>

namespace bench::connections {

using asio::ip::tcp;
static constexpr std::size_t payload_size = 1024;
static constexpr std::size_t peer_workers = 4;
static constexpr std::size_t rounds_per_connection = 32;

struct Payload {
  std::array<std::byte, payload_size> tx{}, rx{};

  void initialize(std::size_t connection)
  {
    for (std::size_t i = 0; i < tx.size(); ++i)
      tx[i] = static_cast<std::byte>((connection * 31 + i * 17) & 255);
    tx[0] = static_cast<std::byte>(connection & 255);
    tx[1] = static_cast<std::byte>((connection >> 8) & 255);
  }
};

struct WeaveConnection : Payload {
  std::optional<weave::TcpStream> socket;
};

static weave::Task<void> weave_roundtrips(WeaveConnection &connection, std::size_t rounds)
{
  for (std::size_t i = 0; i < rounds; ++i) {
    co_await connection.socket->write_all(connection.tx);
    co_await connection.socket->read_exactly(connection.rx);
    if (connection.tx != connection.rx)
      co_await weave::fail(std::errc::bad_message);
  }
}

static asio::awaitable<bool> asio_roundtrips(tcp::socket &socket, Payload &payload, std::size_t rounds)
{
  for (std::size_t i = 0; i < rounds; ++i) {
    auto [sent_error, sent] = co_await asio::async_write(socket, asio::buffer(payload.tx), bench::use_result);
    if (sent_error || sent != payload.tx.size())
      co_return false;
    auto [read_error, read] = co_await asio::async_read(socket, asio::buffer(payload.rx), bench::use_result);
    if (read_error || read != payload.rx.size() || payload.tx != payload.rx)
      co_return false;
  }
  co_return true;
}

static void account(benchmark::State &state)
{
  const auto exchanges = state.iterations() * state.range(0) * rounds_per_connection;
  state.SetItemsProcessed(exchanges);
  state.SetBytesProcessed(exchanges * payload_size * 2);
  state.counters["connections"] = static_cast<weave::f64>(state.range(0));
  state.counters["client_workers"] = static_cast<weave::f64>(state.range(1));
  state.counters["peer_workers"] = peer_workers;
  state.counters["roundtrips_per_connection"] = rounds_per_connection;
}

static void weave_connections(benchmark::State &state, weave::Scheduler scheduler)
{
  const auto count = static_cast<std::size_t>(state.range(0));
  const auto workers = static_cast<std::size_t>(state.range(1));
  support::AsyncEchoPeer peer(peer_workers);
  if (peer.error()) {
    state.SkipWithError("Async echo peer setup failed");
    return;
  }
  std::vector<WeaveConnection> connections(count);
  weave::Runtime runtime({.workers = workers, .scheduler = scheduler});
  if (!runtime.status()) {
    state.SkipWithError("Runtime setup failed");
    return;
  }
  bool setup_ok = true;
  for (std::size_t i = 0; i < count; ++i) {
    connections[i].initialize(i);
    auto setup = runtime.spawn_on(i % workers, [&, i](weave::Context &ctx) -> weave::Task<void> {
      auto socket = co_await weave::tcp::connect(ctx, "127.0.0.1", peer.port(i));
      co_await socket.no_delay();
      connections[i].socket.emplace(std::move(socket));
    });
    weave::detail::require(static_cast<bool>(setup));
    auto result = std::move(*setup).get();
    if (!result) {
      state.SkipWithError(("TCP setup: " + result.error().message()).c_str());
      setup_ok = false;
      break;
    }
  }
  std::vector<weave::JoinHandle<void>> jobs;
  jobs.reserve(count);
  auto batch = [&](std::size_t rounds) {
    for (std::size_t i = 0; i < count; ++i) {
      auto exchange = [&, i, rounds](weave::Context &) { return weave_roundtrips(connections[i], rounds); };
      auto job = scheduler == weave::Scheduler::work_stealing ? runtime.spawn(exchange)
                                                              : runtime.spawn_on(i % workers, exchange);
      weave::detail::require(static_cast<bool>(job));
      jobs.push_back(std::move(*job));
    }
    bool ok = true;
    for (auto &job : jobs)
      ok = std::move(job).get() && ok;
    jobs.clear();
    return ok;
  };
  // Verify all sockets are simultaneously accepted and warm each path before timing.
  if (setup_ok && (!peer.wait_connected(count) || !batch(1))) {
    state.SkipWithError("Connection acceptance or warmup failed");
    setup_ok = false;
  }
  if (setup_ok) {
    for (auto _ : state) {
      if (!batch(rounds_per_connection)) {
        state.SkipWithError("TCP exchange or payload validation failed");
        break;
      }
    }
  }

  // All exchanges have joined. Close the peer first so repeated benchmark setup
  // does not leave thousands of client ephemeral ports in active-close TIME_WAIT.
  peer.stop();
  for (std::size_t i = 0; i < count; ++i) {
    if (!connections[i].socket)
      continue;
    auto cleanup = runtime.spawn_on(i % workers, [&, i](weave::Context &) -> weave::Task<void> {
      connections[i].socket.reset();
      co_return;
    });
    weave::detail::require(static_cast<bool>(cleanup));
    weave::detail::require(static_cast<bool>(std::move(*cleanup).get()));
  }
  runtime.join();
  if (!peer.wait_idle())
    state.SkipWithError("Echo peer failed to drain");
  if (peer.error())
    state.SkipWithError(("Echo peer: " + peer.error().message()).c_str());
  account(state);
}

static void WeaveManyConnections(benchmark::State &state)
{
  weave_connections(state, weave::Scheduler::worker_affine);
}

static void WeaveStealingManyConnections(benchmark::State &state)
{
  weave_connections(state, weave::Scheduler::work_stealing);
}

template <bool Sharded>
static void AsioManyConnections(benchmark::State &state)
{
  const auto count = static_cast<std::size_t>(state.range(0));
  const auto workers = static_cast<std::size_t>(state.range(1));
  support::AsyncEchoPeer peer(peer_workers);
  if (peer.error()) {
    state.SkipWithError("Async echo peer setup failed");
    return;
  }
  bench::AsioPool pool(workers, Sharded);
  std::vector<tcp::socket> sockets;
  sockets.reserve(count);
  std::vector<Payload> payloads(count);
  asio::error_code error;
  for (std::size_t i = 0; i < count; ++i) {
    payloads[i].initialize(i);
    sockets.emplace_back(pool.context(i % workers));
    sockets.back().connect(tcp::endpoint(asio::ip::address_v4::loopback(), peer.port(i)), error);
    if (error) {
      peer.stop();
      state.SkipWithError(("TCP setup: " + error.message()).c_str());
      return;
    }
    sockets.back().set_option(tcp::no_delay(true), error);
    if (error) {
      peer.stop();
      state.SkipWithError(("TCP_NODELAY: " + error.message()).c_str());
      return;
    }
  }
  std::vector<std::future<bool>> jobs;
  jobs.reserve(count);
  auto batch = [&](std::size_t rounds) {
    for (std::size_t i = 0; i < count; ++i) {
      jobs.push_back(
        asio::co_spawn(pool.context(i % workers), asio_roundtrips(sockets[i], payloads[i], rounds), asio::use_future));
    }
    bool ok = true;
    for (auto &job : jobs)
      ok = job.get() && ok;
    jobs.clear();
    return ok;
  };
  if (!peer.wait_connected(count) || !batch(1))
    state.SkipWithError("Connection setup or warmup failed");
  else {
    for (auto _ : state) {
      if (!batch(rounds_per_connection)) {
        state.SkipWithError("TCP exchange or payload validation failed");
        break;
      }
    }
  }
  peer.stop();
  for (auto &socket : sockets) {
    socket.close(error);
    if (error)
      state.SkipWithError("Socket close failed");
  }
  if (!peer.wait_idle())
    state.SkipWithError("Echo peer failed to drain");
  if (peer.error())
    state.SkipWithError(("Echo peer: " + peer.error().message()).c_str());
  account(state);
}

BENCHMARK(WeaveManyConnections)
  ->Args({1024, 1})
  ->Args({1024, 4})
  ->Args({1024, 8})
  ->ArgNames({"connections", "workers"})
  ->UseRealTime()
  ->Unit(benchmark::kMillisecond);
BENCHMARK(WeaveStealingManyConnections)
  ->Args({1024, 1})
  ->Args({1024, 4})
  ->Args({1024, 8})
  ->ArgNames({"connections", "workers"})
  ->UseRealTime()
  ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(AsioManyConnections, false)
  ->Args({1024, 1})
  ->Args({1024, 4})
  ->Args({1024, 8})
  ->ArgNames({"connections", "workers"})
  ->UseRealTime()
  ->Unit(benchmark::kMillisecond);
BENCHMARK_TEMPLATE(AsioManyConnections, true)
  ->Args({1024, 1})
  ->Args({1024, 4})
  ->Args({1024, 8})
  ->ArgNames({"connections", "workers"})
  ->UseRealTime()
  ->Unit(benchmark::kMillisecond);

} // namespace bench::connections
