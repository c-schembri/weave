#include <doctest/doctest.h>
#include <weave/tcp.hpp>
#include <weave/runtime.hpp>
#include "async_echo_peer.hpp"
#include "runtime_fixture.hpp"

namespace test_task_concurrent {

struct Session {
  std::atomic<unsigned> live = 0, destroyed = 0;
  std::error_code error;
  unsigned exchanges = 0, recovered = 0;
  bool resumed_after_failure = false, observed = false;
};

struct Guard {
  Session &session;

  explicit Guard(Session &s) : session(s)
  {
    ++session.live;
  }

  ~Guard()
  {
    --session.live;
    ++session.destroyed;
  }
};

struct Gate {
  std::atomic<unsigned> ready = 0, finished = 0, exchanged = 0;
  std::atomic<bool> open = false;
};

template <class P>
static bool wait_for(P predicate)
{
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= end)
      return false;
    Sleep(1);
  }
  return true;
}

static weave::Task<void> observe(weave::Task<void> task, Session &session, Gate &gate)
{
  auto result = co_await weave::as_result(std::move(task));
  session.error = result ? std::error_code{} : result.error();
  session.observed = true;
  CHECK(session.live == 0);
  ++gate.finished;
}

static weave::Task<void> exchange(
  weave::TcpStream &socket,
  std::span<std::byte> tx,
  std::span<std::byte> rx,
  Session &session,
  bool fail,
  unsigned depth)
{
  Guard guard{session};
  if (depth) {
    co_await exchange(socket, tx, rx, session, fail, depth - 1);
  } else {
    co_await socket.write_all(tx);
    co_await socket.read_exactly(rx);
    if (!std::equal(tx.begin(), tx.end(), rx.begin()))
      co_await weave::fail(std::errc::bad_message);
    ++session.exchanges;
    if (fail)
      co_await weave::fail(std::errc::permission_denied);
  }
  if (fail)
    session.resumed_after_failure = true;
}

static weave::Task<void> echo_session(weave::Context &ctx, weave::u16 port, unsigned id, Session &session, Gate &gate)
{
  Guard guard{session};
  auto socket = co_await weave::tcp::connect(ctx, "127.0.0.1", port);
  if (auto status = socket.no_delay(); !status)
    co_await weave::fail(status.error());
  std::array<std::byte, 2048> tx{}, rx{};
  for (std::size_t i = 0; i < tx.size(); ++i)
    tx[i] = static_cast<std::byte>((id * 31 + i * 17) & 255);
  ++gate.ready;
  while (!gate.open.load())
    co_await ctx.yield();
  for (unsigned i = 0; i < 8; ++i) {
    const bool fail = i % 3 == 0;
    auto result = co_await weave::as_result(exchange(socket, tx, rx, session, fail, 3));
    CHECK(session.live == 1);
    if (fail) {
      if (result || result.error() != std::errc::permission_denied)
        co_await weave::fail(std::errc::bad_message);
      ++session.recovered;
    } else if (!result) {
      co_await weave::fail(result.error());
    }
    co_await ctx.yield();
  }
  ++gate.exchanged;
  if ((co_await socket.read(rx)) != 0)
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> pending_read(weave::TcpStream &socket, std::span<std::byte> buffer, Session &session)
{
  Guard guard{session};
  (void)co_await socket.read(buffer);
  session.resumed_after_failure = true;
}

static weave::Task<void> cancel_sibling(weave::Context &ctx, weave::TcpStream &socket)
{
  for (int i = 0; i < 3; ++i)
    co_await ctx.yield();
  if (auto status = socket.cancel(); !status)
    co_await weave::fail(status.error());
  co_await weave::fail(std::errc::io_error);
}

static weave::Task<void> cancelled_session(
  weave::Context &ctx,
  weave::u16 port,
  Session &session,
  Gate &gate,
  bool sibling)
{
  Guard guard{session};
  auto socket = co_await weave::tcp::connect(ctx, "127.0.0.1", port);
  std::array<std::byte, 8192> buffer{};
  ++gate.ready;
  if (sibling)
    co_await weave::when_all(pending_read(socket, buffer, session), cancel_sibling(ctx, socket));
  else
    co_await pending_read(socket, buffer, session);
  session.resumed_after_failure = true;
}

TEST_CASE_TEMPLATE(
  "1024 live connections recover nested failures under both schedulers",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  constexpr unsigned count = 1024;
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    for (bool skip : {false, true}) {
      support::AsyncEchoPeer peer(4);
      REQUIRE_FALSE(peer.error());
      std::vector<Session> sessions(count);
      Gate gate;
      auto runtime = support::create_runtime<Layout>(
        {.workers = 4, .scheduler = scheduler, .context = {.skip_successful_completions = skip}});
      REQUIRE(runtime);
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned i = 0; i < count; ++i) {
        auto job = runtime->spawn([&, i](weave::Context &ctx) {
          return observe(echo_session(ctx, peer.port(i), i, sessions[i], gate), sessions[i], gate);
        });
        REQUIRE(job);
        jobs.push_back(std::move(*job));
      }
      const bool ready = wait_for([&] { return gate.ready == count || gate.finished != 0; }) && gate.ready == count;
      gate.open = true;
      if (!ready)
        runtime->request_stop();
      const bool exchanged = ready && wait_for([&] { return gate.exchanged == count || gate.finished != 0; }) &&
        gate.exchanged == count;
      peer.stop();
      for (auto &job : jobs)
        REQUIRE(std::move(job).get());
      runtime->join();
      REQUIRE(ready);
      REQUIRE(exchanged);
      CHECK(gate.finished == count);
      for (const auto &session : sessions) {
        CHECK(session.observed);
        // The fixture closes with an outstanding receive, so Windows may reset
        // instead of delivering a graceful FIN. Both must cleanly end the Task.
        CHECK((!session.error || session.error.value() == WSAECONNRESET));
        CHECK(session.exchanges == 8);
        CHECK(session.recovered == 3);
        CHECK(session.live == 0);
        CHECK(session.destroyed == 33);
        CHECK_FALSE(session.resumed_after_failure);
      }
      CHECK(peer.wait_idle());
      CHECK_FALSE(peer.error());
    }
  }
}

TEST_CASE_TEMPLATE(
  "Concurrent sibling failure retains borrowed buffers until cancelled IOCP reads drain",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  constexpr unsigned count = 256;
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    for (bool skip : {false, true}) {
      support::AsyncEchoPeer peer(4);
      REQUIRE_FALSE(peer.error());
      std::vector<Session> sessions(count);
      Gate gate;
      auto runtime = support::create_runtime<Layout>(
        {.workers = 4, .scheduler = scheduler, .context = {.skip_successful_completions = skip}});
      REQUIRE(runtime);
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned i = 0; i < count; ++i) {
        auto job = runtime->spawn([&, i](weave::Context &ctx) {
          return observe(cancelled_session(ctx, peer.port(i), sessions[i], gate, true), sessions[i], gate);
        });
        REQUIRE(job);
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs)
        REQUIRE(std::move(job).get());
      runtime->join();
      CHECK(gate.finished == count);
      for (const auto &session : sessions) {
        CHECK(session.error.value() == ERROR_OPERATION_ABORTED);
        CHECK(session.observed);
        CHECK(session.live == 0);
        CHECK(session.destroyed == 2);
        CHECK_FALSE(session.resumed_after_failure);
      }
      CHECK(peer.wait_idle());
      CHECK_FALSE(peer.error());
    }
  }
}

TEST_CASE_TEMPLATE(
  "Multicore shutdown drains pending TCP reads even when half the join handles were dropped",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  constexpr unsigned count = 256;
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    for (bool skip : {false, true}) {
      support::AsyncEchoPeer peer(4);
      REQUIRE_FALSE(peer.error());
      std::vector<Session> sessions(count);
      Gate gate;
      auto runtime = support::create_runtime<Layout>(
        {.workers = 4, .scheduler = scheduler, .context = {.skip_successful_completions = skip}});
      REQUIRE(runtime);
      std::array<std::array<weave::u64, 2>, 4> metrics{};
      std::vector<weave::JoinHandle<void>> inspectors;
      for (unsigned i = 0; i < metrics.size(); ++i) {
        auto inspect = runtime->spawn_on(i, [&, i](weave::Context &ctx) -> weave::Task<void> {
          while (gate.finished != count)
            co_await ctx.yield();
          metrics[i] = {ctx.metrics().submitted, ctx.metrics().completed};
        });
        REQUIRE(inspect);
        inspectors.push_back(std::move(*inspect));
      }
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned i = 0; i < count; ++i) {
        auto job = runtime->spawn([&, i](weave::Context &ctx) {
          return observe(cancelled_session(ctx, peer.port(i), sessions[i], gate, false), sessions[i], gate);
        });
        REQUIRE(job);
        if (i % 2)
          jobs.push_back(std::move(*job));
      }
      const bool ready = wait_for([&] { return gate.ready == count || gate.finished != 0; }) && gate.ready == count;
      runtime->shutdown();
      for (auto &job : jobs)
        REQUIRE(std::move(job).get());
      REQUIRE(ready);
      CHECK(gate.finished == count);
      for (const auto &session : sessions) {
        CHECK(session.error == std::errc::operation_canceled);
        CHECK(session.observed);
        CHECK(session.live == 0);
        CHECK(session.destroyed == 2);
        CHECK_FALSE(session.resumed_after_failure);
      }
      for (auto &inspector : inspectors)
        REQUIRE(std::move(inspector).get());
      for (const auto &metric : metrics)
        CHECK(metric[0] == metric[1]);
      CHECK(peer.wait_idle());
      CHECK_FALSE(peer.error());
    }
  }
}

} // namespace test_task_concurrent
