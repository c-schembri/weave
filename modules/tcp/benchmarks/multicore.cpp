#include <weave/tcp.hpp>
#include <weave/runtime.hpp>
#include "common.hpp"
#include "asio_pool.hpp"
#include "echo_peer.hpp"
#include <future>

namespace bench::multicore {

static constexpr std::size_t jobs_per_batch = 32;
static constexpr std::size_t payload_size = 1024;
using asio::ip::tcp;
using bench::AsioPool;

struct Payload {
  std::array<std::byte, payload_size> tx{}, rx{};

  Payload()
  {
    tx.fill(std::byte{0x59});
  }
};

struct WeaveConnection : Payload {
  std::optional<weave::TcpStream> socket;
};

static weave::Task<void> weave_exchange(WeaveConnection &connection)
{
  co_await connection.socket->write_all(connection.tx);
  co_await connection.socket->read_exactly(connection.rx);
}

static asio::awaitable<bool> asio_exchange(tcp::socket &socket, Payload &payload)
{
  auto [sent_error, sent] = co_await asio::async_write(socket, asio::buffer(payload.tx), bench::use_result);
  if (sent_error || sent != payload.tx.size())
    co_return false;
  auto [read_error, read] = co_await asio::async_read(socket, asio::buffer(payload.rx), bench::use_result);
  co_return !read_error && read == payload.rx.size();
}

static void tcp_account(benchmark::State &state)
{
  state.SetItemsProcessed(state.iterations() * jobs_per_batch);
  state.SetBytesProcessed(state.iterations() * jobs_per_batch * payload_size * 2);
}

static void weave_tcp_benchmark(benchmark::State &state, weave::Scheduler scheduler)
{
  std::vector<std::unique_ptr<support::EchoPeer>> peers;
  std::array<WeaveConnection, jobs_per_batch> connections;
  const auto workers = static_cast<std::size_t>(state.range(0));
  weave::Runtime runtime({.workers = workers, .scheduler = scheduler});
  if (!runtime.status()) {
    state.SkipWithError("Runtime setup failed");
    return;
  }
  bool setup_ok = true;
  for (std::size_t i = 0; i < jobs_per_batch; ++i) {
    peers.push_back(std::make_unique<support::EchoPeer>());
    auto setup = runtime.spawn_on(i % workers, [&, i](weave::Context &ctx) -> weave::Task<void> {
      auto socket = co_await weave::tcp::connect(ctx, "127.0.0.1", peers[i]->port());
      co_await socket.no_delay();
      connections[i].socket.emplace(std::move(socket));
    });
    weave::detail::require(static_cast<bool>(setup));
    if (!std::move(*setup).get()) {
      setup_ok = false;
      break;
    }
  }
  std::vector<weave::JoinHandle<void>> jobs;
  jobs.reserve(jobs_per_batch);
  if (!setup_ok)
    state.SkipWithError("TCP setup failed");
  else
    for (auto _ : state) {
      for (std::size_t i = 0; i < jobs_per_batch; ++i) {
        auto exchange = [&, i](weave::Context &) { return weave_exchange(connections[i]); };
        auto job = scheduler == weave::Scheduler::work_stealing ? runtime.spawn(exchange)
                                                                : runtime.spawn_on(i % workers, exchange);
        weave::detail::require(static_cast<bool>(job));
        jobs.push_back(std::move(*job));
      }
      bool ok = true;
      for (auto &job : jobs)
        ok = std::move(job).get() && ok;
      jobs.clear();
      if (!ok) {
        state.SkipWithError("TCP exchange failed");
        break;
      }
      benchmark::ClobberMemory();
    }
  // Persistent sockets must be destroyed on their owning workers, even on failure.
  for (std::size_t i = 0; i < jobs_per_batch; ++i) {
    if (!connections[i].socket)
      continue;
    if (setup_ok && connections[i].tx != connections[i].rx)
      state.SkipWithError("Payload mismatch");
    auto cleanup = runtime.spawn_on(i % workers, [&, i](weave::Context &) -> weave::Task<void> {
      connections[i].socket.reset();
      co_return;
    });
    weave::detail::require(static_cast<bool>(cleanup));
    weave::detail::require(static_cast<bool>(std::move(*cleanup).get()));
  }
  runtime.join();
  for (auto &peer : peers) {
    // If connect failed, the fixture destructor closes its pending accept.
    if (setup_ok)
      peer->join();
    if (!peer->ok())
      state.SkipWithError("Echo peer failed");
  }
  tcp_account(state);
}

static void WeaveMulticoreTcp(benchmark::State &state)
{
  weave_tcp_benchmark(state, weave::Scheduler::worker_affine);
}

static void WeaveStealingTcp(benchmark::State &state)
{
  weave_tcp_benchmark(state, weave::Scheduler::work_stealing);
}

template <bool Sharded>
static void AsioMulticoreTcp(benchmark::State &state)
{
  std::vector<std::unique_ptr<support::EchoPeer>> peers;
  const auto workers = static_cast<std::size_t>(state.range(0));
  AsioPool pool(workers, Sharded);
  std::vector<tcp::socket> sockets;
  std::array<Payload, jobs_per_batch> payloads;
  asio::error_code error;
  for (std::size_t i = 0; i < jobs_per_batch; ++i) {
    peers.push_back(std::make_unique<support::EchoPeer>());
    sockets.emplace_back(pool.context(i % workers));
    sockets.back().connect(tcp::endpoint(asio::ip::address_v4::loopback(), peers.back()->port()), error);
    if (error) {
      state.SkipWithError("TCP setup failed");
      return;
    }
    sockets.back().set_option(tcp::no_delay(true), error);
    if (error) {
      state.SkipWithError("TCP_NODELAY failed");
      return;
    }
  }
  std::vector<std::future<bool>> jobs;
  jobs.reserve(jobs_per_batch);
  for (auto _ : state) {
    for (std::size_t i = 0; i < jobs_per_batch; ++i) {
      jobs.push_back(
        asio::co_spawn(pool.context(i % workers), asio_exchange(sockets[i], payloads[i]), asio::use_future));
    }
    bool ok = true;
    for (auto &job : jobs)
      ok = job.get() && ok;
    jobs.clear();
    if (!ok) {
      state.SkipWithError("TCP exchange failed");
      break;
    }
    benchmark::ClobberMemory();
  }
  for (std::size_t i = 0; i < jobs_per_batch; ++i) {
    if (payloads[i].tx != payloads[i].rx)
      state.SkipWithError("Payload mismatch");
    sockets[i].close(error);
    if (error)
      state.SkipWithError("Socket close failed");
    peers[i]->join();
    if (!peers[i]->ok())
      state.SkipWithError("Echo peer failed");
  }
  tcp_account(state);
}

BENCHMARK(WeaveMulticoreTcp)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK(WeaveStealingTcp)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->UseRealTime()->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(AsioMulticoreTcp, false)
  ->Arg(1)
  ->Arg(2)
  ->Arg(4)
  ->Arg(8)
  ->UseRealTime()
  ->Unit(benchmark::kMicrosecond);
BENCHMARK_TEMPLATE(AsioMulticoreTcp, true)
  ->Arg(1)
  ->Arg(2)
  ->Arg(4)
  ->Arg(8)
  ->UseRealTime()
  ->Unit(benchmark::kMicrosecond);

} // namespace bench::multicore
