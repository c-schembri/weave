#include "common.hpp"
#include "callback_pool.hpp"
#include "echo_peer.hpp"
#include "async_echo_peer.hpp"

using support::LibuvClient;
using support::UsocketsClient;

static void initialize(std::vector<std::byte> &tx, bool many, std::size_t connection, bool serial, bool multicore)
{
  for (std::size_t i = 0; i < tx.size(); ++i) {
    std::size_t value;
    if (many)
      value = (connection * 31 + i * 17) & 255;
    else if (serial)
      value = 0x5a;
    else if (multicore)
      value = 0x59;
    else
      value = i % 251;

    tx[i] = static_cast<std::byte>(value);
  }

  if (many) {
    tx[0] = static_cast<std::byte>(connection & 255);
    tx[1] = static_cast<std::byte>((connection >> 8) & 255);
  }
}

template <class Client, std::size_t Count, bool Bulk>
static void native_local(benchmark::State &state)
{
  std::vector<std::unique_ptr<support::EchoPeer>> peers;
  support::CallbackGroup<Client> group(Count, static_cast<std::size_t>(state.range(0)));
  for (std::size_t i = 0; i < Count; ++i) {
    peers.push_back(std::make_unique<support::EchoPeer>());
    initialize(group.clients[i]->tx, false, i, Count == 1 && !Bulk, false);
    if (!group.connect(i, peers[i]->port())) {
      state.SkipWithError("Native TCP connect failed");
      return;
    }
  }

  struct Driver {
    benchmark::State &state;
    support::CallbackGroup<Client> &group;
    std::size_t remaining = 0;
    bool ok = true;

    void next()
    {
      if (!ok) {
        state.SkipWithError("Native TCP exchange failed");
        group.loop.stop();
      } else if (!state.KeepRunning()) {
        group.loop.stop();
      } else {
        remaining = Count;
        support::Callback completed{this, [](void *data, bool success) {
                                      auto &driver = *static_cast<Driver *>(data);
                                      driver.ok = success && driver.ok;
                                      if (--driver.remaining == 0) {
                                        benchmark::ClobberMemory();
                                        driver.next();
                                      }
                                    }};

        for (auto &client : group.clients)
          client->exchange(1, false, completed);
      }
    }
  } driver{state, group};

  support::CallbackLoop::Command start{&driver, [](void *data) { static_cast<Driver *>(data)->next(); }};
  group.loop.post(start);
  group.loop.run();
  for (const auto &client : group.clients) {
    if (client->tx != client->rx)
      state.SkipWithError("Native payload mismatch");
  }
  group.close();
  for (auto &peer : peers) {
    peer->join();
    if (!peer->ok())
      state.SkipWithError("Echo peer failed");
  }
  state.SetItemsProcessed(state.iterations() * Count);
  state.SetBytesProcessed(state.iterations() * Count * state.range(0) * 2);
  if constexpr (Count != 1)
    state.counters["connections"] = Count;
}

template <class Client, bool Many>
static void native_tcp(benchmark::State &state)
{
  const auto count = static_cast<std::size_t>(Many ? state.range(0) : 32);
  const auto workers = static_cast<std::size_t>(state.range(Many ? 1 : 0));
  constexpr std::size_t size = 1024, rounds = Many ? 32 : 1;
  std::unique_ptr<support::AsyncEchoPeer> async_peer;
  std::vector<std::unique_ptr<support::EchoPeer>> peers;
  if constexpr (Many) {
    async_peer = std::make_unique<support::AsyncEchoPeer>(4);
    if (async_peer->error()) {
      state.SkipWithError("Async echo peer setup failed");
      return;
    }
  }
  support::CallbackPool<Client> pool(workers, count, size);
  bool setup_ok = true;
  for (std::size_t i = 0; i < count; ++i) {
    weave::u16 port;
    if constexpr (Many)
      port = async_peer->port(i);
    else {
      peers.push_back(std::make_unique<support::EchoPeer>());
      port = peers.back()->port();
    }
    initialize(pool.client(i).tx, Many, i, false, true);
    if (!pool.connect(i, port).get()) {
      setup_ok = false;
      state.SkipWithError(uv_strerror(pool.client(i).error()));
      break;
    }
  }
  std::vector<std::future<bool>> jobs;
  jobs.reserve(count);
  auto batch = [&](std::size_t repetitions) {
    for (std::size_t i = 0; i < count; ++i)
      jobs.push_back(pool.exchange(i, repetitions, Many));
    bool ok = true;
    for (auto &job : jobs)
      ok = job.get() && ok;
    jobs.clear();
    return ok;
  };
  if constexpr (Many) {
    if (setup_ok && (!async_peer->wait_connected(count) || !batch(1))) {
      setup_ok = false;
      state.SkipWithError("Connection acceptance or warmup failed");
    }
  }
  if (setup_ok) {
    for (auto _ : state) {
      if (!batch(rounds)) {
        state.SkipWithError("Native TCP exchange or validation failed");
        break;
      }
      benchmark::ClobberMemory();
    }
  }
  if constexpr (Many)
    async_peer->stop();
  pool.stop();
  for (std::size_t i = 0; i < count && setup_ok; ++i) {
    if (pool.client(i).tx != pool.client(i).rx)
      state.SkipWithError("Native payload mismatch");
  }
  if constexpr (Many) {
    if (!async_peer->wait_idle() || async_peer->error())
      state.SkipWithError("Async echo peer failed to drain");
    state.counters["connections"] = static_cast<weave::f64>(count);
    state.counters["client_workers"] = static_cast<weave::f64>(workers);
    state.counters["peer_workers"] = 4;
    state.counters["roundtrips_per_connection"] = rounds;
  } else {
    for (auto &peer : peers) {
      if (setup_ok)
        peer->join();
      if (!peer->ok())
        state.SkipWithError("Echo peer failed");
    }
  }
  state.SetItemsProcessed(state.iterations() * count * rounds);
  state.SetBytesProcessed(state.iterations() * count * rounds * size * 2);
}

#define REGISTER_NATIVE(Client, Prefix)               \
  BENCHMARK_TEMPLATE(native_local, Client, 1, false)  \
    ->Name(Prefix)                                    \
    ->Arg(64)                                         \
    ->Arg(1024)                                       \
    ->Arg(65536)                                      \
    ->UseRealTime()                                   \
    ->Unit(benchmark::kMicrosecond);                  \
  BENCHMARK_TEMPLATE(native_local, Client, 1, true)   \
    ->Name(Prefix "Bulk")                             \
    ->Arg(1 << 20)                                    \
    ->Arg(8 << 20)                                    \
    ->UseRealTime()                                   \
    ->Unit(benchmark::kMicrosecond);                  \
  BENCHMARK_TEMPLATE(native_local, Client, 8, false)  \
    ->Name(Prefix "Concurrent<8>")                    \
    ->Arg(1024)                                       \
    ->UseRealTime()                                   \
    ->Unit(benchmark::kMicrosecond);                  \
  BENCHMARK_TEMPLATE(native_local, Client, 32, false) \
    ->Name(Prefix "Concurrent<32>")                   \
    ->Arg(1024)                                       \
    ->UseRealTime()                                   \
    ->Unit(benchmark::kMicrosecond);                  \
  BENCHMARK_TEMPLATE(native_tcp, Client, false)       \
    ->Name(Prefix "MulticoreTcp")                     \
    ->Arg(1)                                          \
    ->Arg(2)                                          \
    ->Arg(4)                                          \
    ->Arg(8)                                          \
    ->UseRealTime()                                   \
    ->Unit(benchmark::kMicrosecond);                  \
  BENCHMARK_TEMPLATE(native_tcp, Client, true)        \
    ->Name(Prefix "ManyConnections")                  \
    ->Args({1024, 1})                                 \
    ->Args({1024, 4})                                 \
    ->Args({1024, 8})                                 \
    ->ArgNames({"connections", "workers"})            \
    ->UseRealTime()                                   \
    ->Unit(benchmark::kMillisecond)

REGISTER_NATIVE(LibuvClient, "Libuv");
REGISTER_NATIVE(UsocketsClient, "Usockets");
#undef REGISTER_NATIVE
