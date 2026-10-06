#include <doctest/doctest.h>
#include <weave/tcp.hpp>
#include <weave/runtime.hpp>
#include "echo_peer.hpp"
#include <memory>
#include <vector>

using weave::Result;
using weave::Task;
static_assert(std::same_as<decltype(std::declval<weave::TcpStream &>().read({})), Task<std::size_t>>);
static_assert(std::same_as<decltype(std::declval<weave::TcpStream &>().write_all({})), Task<void>>);
static_assert(std::same_as<decltype(std::declval<weave::TcpListener &>().accept()), Task<weave::TcpStream>>);
static_assert(std::same_as<decltype(std::declval<weave::JoinHandle<int>>().get()), Result<int>>);
static_assert(std::same_as<decltype(std::declval<weave::JoinHandle<void>>().get()), Result<void>>);

namespace test_task {

struct Guard {
  std::vector<int> &log;
  int id;

  ~Guard()
  {
    log.push_back(id);
  }
};

static Task<int> answer(int &calls)
{
  ++calls;
  co_return 42;
}

static Task<int> failing_chain(weave::Context &ctx, int depth, bool delayed, std::vector<int> &log, int &unreachable)
{
  Guard guard{log, depth};
  if (!depth) {
    if (delayed)
      co_await ctx.yield();
    co_await weave::fail(std::errc::io_error);
  }
  auto value = co_await failing_chain(ctx, depth - 1, delayed, log, unreachable);
  ++unreachable;
  co_return value;
}

static Task<void> void_failure(int &unreachable)
{
  co_await weave::fail(std::errc::permission_denied);
  ++unreachable;
}

static Task<int> returned_failure()
{
  co_return std::unexpected(std::make_error_code(std::errc::invalid_argument));
}

static Task<void> void_parent(int &unreachable)
{
  (void)co_await returned_failure();
  ++unreachable;
}

static weave::Task<int> native_failure(weave::Context &ctx, bool delayed)
{
  if (delayed)
    co_await ctx.yield();
  co_return std::unexpected(std::make_error_code(std::errc::io_error));
}

static Task<int> native_parent(weave::Context &ctx, bool delayed, int &unreachable)
{
  auto n = co_await native_failure(ctx, delayed);
  ++unreachable;
  co_return n;
}

TEST_CASE("Task is lazy, move-only, and transfers values")
{
  static_assert(!std::is_copy_constructible_v<Task<int>>);
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  int calls = 0;
  {
    auto unused = answer(calls);
  }
  CHECK(calls == 0);
  auto task = answer(calls);
  auto moved = std::move(task);
  auto result = ctx->run(std::move(moved));
  REQUIRE(result);
  CHECK(*result == 42);
  CHECK(calls == 1);
}

TEST_CASE("Immediate and delayed failure skip every parent and destroy inside-out")
{
  for (bool delayed : {false, true}) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);

    std::vector<int> log;
    int unreachable = 0;
    auto result = ctx->run(failing_chain(*ctx, 32, delayed, log, unreachable));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::io_error);
    CHECK(unreachable == 0);
    REQUIRE(log.size() == 33);
    for (int i = 0; i <= 32; ++i)
      CHECK(log[i] == i);
  }
}

TEST_CASE("Void and non-void error sources propagate without exceptions")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  int unreachable = 0;
  auto a = ctx->run(void_failure(unreachable));
  auto b = ctx->run(void_parent(unreachable));
  REQUIRE_FALSE(a);
  REQUIRE_FALSE(b);
  CHECK(a.error() == std::errc::permission_denied);
  CHECK(b.error() == std::errc::invalid_argument);
  CHECK(unreachable == 0);
  auto zero_error = []() -> Task<void> {
    co_await weave::fail(weave::Error{});
  };
  CHECK_FALSE(ctx->run(zero_error()));
}

TEST_CASE("Recovery reclaims the complete failed chain before executing the handler")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  std::vector<int> log;
  int unreachable = 0, calls = 0;
  auto recover = [&]() -> Task<int> {
    auto error = co_await weave::as_result(failing_chain(*ctx, 10, true, log, unreachable));
    CHECK_FALSE(error);
    CHECK(log.size() == 11);
    auto a = co_await answer(calls);
    auto b = co_await answer(calls);
    co_return a + b;
  };
  auto result = ctx->run(recover());
  REQUIRE(result);
  CHECK(*result == 84);
  CHECK(calls == 2);
  CHECK(unreachable == 0);
}

TEST_CASE("Multiple awaits in one expression do not retain stale ownership links")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  int calls = 0;
  auto sum = [&]() -> Task<int> {
    co_return (co_await answer(calls)) + (co_await answer(calls));
  };
  CHECK(ctx->run(sum()) == Result<int>{84});
  CHECK(calls == 2);
}

TEST_CASE("Move-only tasks compose with explicitly checked synchronous Results")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto child = []() -> Task<std::unique_ptr<int>> {
    co_return std::make_unique<int>(73);
  };
  auto parent = [&]() -> Task<std::unique_ptr<int>> {
    auto n = Result<int>{3};
    if (!n)
      co_await weave::fail(n.error());
    auto pointer = co_await child();
    *pointer += *n;
    co_return pointer;
  };
  auto result = ctx->run(parent());
  REQUIRE(result);
  CHECK(**result == 76);
  auto failure = []() -> Task<void> {
    auto status = Result<void>{std::unexpected(std::make_error_code(std::errc::io_error))};
    if (!status)
      co_await weave::fail(status.error());
    FAIL("Resumed after synchronous failure");
  };
  CHECK_FALSE(ctx->run(failure()));
}

TEST_CASE("Nested Task failure supports immediate and pending recovery")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  int unreachable = 0;
  for (bool delayed : {false, true}) {
    auto result = ctx->run(native_parent(*ctx, delayed, unreachable));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::io_error);
  }
  CHECK(unreachable == 0);
  auto recover = [&]() -> Task<int> {
    auto result = co_await weave::as_result(native_failure(*ctx, true));
    CHECK_FALSE(result);
    co_return 19;
  };
  CHECK(ctx->run(recover()) == Result<int>{19});
}

TEST_CASE("Deep immediate and delayed failure have bounded-stack routing and destruction")
{
  for (bool delayed : {false, true}) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);

    std::vector<int> log;
    log.reserve(20001);
    int unreachable = 0;
    auto result = ctx->run(failing_chain(*ctx, 20000, delayed, log, unreachable));
    REQUIRE_FALSE(result);
    CHECK(unreachable == 0);
    REQUIRE(log.size() == 20001);
    for (int i = 0; i <= 20000; ++i) {
      if (log[i] != i) {
        FAIL("Incorrect destruction order");
        break;
      }
    }
  }
}

TEST_CASE("Join-all waits for delayed siblings and selects errors by argument order")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  int finished = 0, unreachable = 0;
  auto slow = [&]() -> Task<void> {
    for (int i = 0; i < 20; ++i)
      co_await ctx->yield();
    ++finished;
    co_await weave::fail(std::errc::address_in_use);
  };
  auto fast = [&]() -> Task<void> {
    co_await weave::fail(std::errc::io_error);
  };
  auto parent = [&]() -> Task<void> {
    co_await weave::when_all(slow(), fast());
    ++unreachable;
  };
  auto result = ctx->run(parent());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::address_in_use);
  CHECK(finished == 1);
  CHECK(unreachable == 0);
  CHECK(ctx->run(weave::when_all()));
}

TEST_CASE("Socket immediate failures and ConnectEx refusal propagate")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto invalid = [&]() -> Task<void> {
    (void)co_await weave::tcp::connect(*ctx, "bad host", 80);
    FAIL("Resumed after invalid address");
  };
  CHECK_FALSE(ctx->run(invalid()));
  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port != 0);
  REQUIRE(listener->close());
  auto refused = [&]() -> Task<void> {
    (void)co_await weave::tcp::connect(*ctx, "127.0.0.1", port);
    FAIL("Resumed after refused connection");
  };
  CHECK_FALSE(ctx->run(refused()));
  auto closed = [&]() -> Task<void> {
    (void)co_await listener->accept();
    FAIL("Resumed after closed listener");
  };
  CHECK_FALSE(ctx->run(closed()));
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

TEST_CASE("TCP partial receives, large concurrent transfers, EOF and closed streams")
{
  for (bool skip : {false, true}) {
    support::EchoPeer peer(997);
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);

    auto socket = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", peer.port()));
    REQUIRE(socket);
    REQUIRE(socket->no_delay());
    std::vector<std::byte> tx(4 * 1024 * 1024), rx(tx.size());
    for (std::size_t i = 0; i < tx.size(); ++i)
      tx[i] = static_cast<std::byte>(i % 251);
    auto writer = [&]() -> Task<void> {
      co_await socket->write_all(tx);
    };
    auto reader = [&]() -> Task<void> {
      co_await socket->read_exactly(rx);
    };
    REQUIRE(ctx->run(weave::when_all(writer(), reader())));
    CHECK(tx == rx);
    auto finish = [&]() -> Task<void> {
      co_await socket->write_all({});
      CHECK((co_await socket->read({})) == 0);
      if (auto status = socket->shutdown_send(); !status)
        co_await weave::fail(status.error());
      std::array<std::byte, 1> tail{};
      CHECK((co_await socket->read(tail)) == 0);
      co_await socket->read_exactly(tail);
      FAIL("Unexpected EOF must short-circuit read_exactly");
    };
    CHECK_FALSE(ctx->run(finish()));
    REQUIRE(socket->close());
    auto closed = [&]() -> Task<void> {
      std::array<std::byte, 1> buffer{};
      co_await socket->write_all(buffer);
      FAIL("Resumed after closed socket");
    };
    CHECK_FALSE(ctx->run(closed()));
    peer.join();
    CHECK(peer.ok());
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
  }
}

TEST_CASE("A sibling failure cannot free a cancelled read before its IOCP packet")
{
  for (bool skip : {false, true}) {
    support::EchoPeer peer;
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);

    auto socket = ctx->run(weave::tcp::connect(*ctx, "127.0.0.1", peer.port()));
    REQUIRE(socket);
    std::vector<int> log;
    auto pending = [&]() -> Task<void> {
      Guard guard{log, 1};
      std::array<std::byte, 8192> buffer{};
      (void)co_await socket->read(buffer);
      FAIL("Cancelled read must not resume its Task body");
    };
    auto cancel = [&]() -> Task<void> {
      CHECK(log.empty());
      std::array<std::byte, 1> buffer{};
      auto busy = co_await weave::as_result(socket->read(buffer));
      CHECK_FALSE(busy);
      CHECK(busy.error() == std::errc::operation_in_progress);
      CHECK_FALSE(socket->close());
      if (auto status = socket->cancel(); !status)
        co_await weave::fail(status.error());
      CHECK(log.empty());
      co_await weave::fail(std::errc::io_error);
    };
    auto result = ctx->run(weave::when_all(pending(), cancel()));
    REQUIRE_FALSE(result);
    CHECK(result.error().value() == support::native_cancelled);
    CHECK(log == std::vector<int>{1});
    CHECK(ctx->metrics().submitted == ctx->metrics().completed);
    CHECK(socket->close());
    peer.join();
    CHECK(peer.ok());
  }
}

static Task<void> echo(weave::TcpStream &client)
{
  std::array<std::byte, 4096> buffer;
  for (;;) {
    auto n = co_await client.read(buffer);
    if (n == 0)
      co_return;
    co_await client.write_all(std::span(buffer).first(n));
  }
}

TEST_CASE("The requested echo syntax works with AcceptEx and a clean half-close")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 0);
  REQUIRE(listener);
  auto port = listener->local_port();
  REQUIRE(port != 0);
  auto server = [&]() -> Task<void> {
    auto socket = co_await listener->accept();
    co_await echo(socket);
  };
  auto client = [&]() -> Task<void> {
    auto socket = co_await weave::tcp::connect(*ctx, "127.0.0.1", port);
    std::array<std::byte, 8192> tx{}, rx{};
    tx.fill(std::byte{0x59});
    co_await socket.write_all(tx);
    if (auto status = socket.shutdown_send(); !status)
      co_await weave::fail(status.error());
    co_await socket.read_exactly(rx);
    CHECK(tx == rx);
    CHECK((co_await socket.read(rx)) == 0);
  };
  CHECK(ctx->run(weave::when_all(server(), client())));
  CHECK(ctx->metrics().submitted == ctx->metrics().completed);
}

static Task<int> worker_task(weave::Context &ctx, int id, std::atomic<int> &destroyed)
{
  struct Count {
    std::atomic<int> &value;

    ~Count()
    {
      ++value;
    }
  } guard{destroyed};

  for (int i = 0; i < 8; ++i)
    co_await ctx.yield();
  if (id % 2)
    co_await weave::fail(std::errc::io_error);
  co_return id;
}

TEST_CASE("Runtime roots preserve errors and cleanup under both schedulers")
{
  for (auto mode : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = mode});
    REQUIRE(runtime);
    std::atomic<int> destroyed = 0;
    std::vector<weave::JoinHandle<int>> jobs;
    for (int i = 0; i < 256; ++i) {
      auto job = runtime->spawn([i, &destroyed](weave::Context &ctx) {
        return worker_task(ctx, i, destroyed);
      });
      REQUIRE(job);
      if (i < 128)
        jobs.push_back(std::move(*job));
    }
    for (int i = 0; i < 128; ++i) {
      auto result = std::move(jobs[i]).get();
      if (i % 2) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == std::errc::io_error);
      } else {
        CHECK(result == Result<int>{i});
      }
    }
    runtime->join();
    CHECK(destroyed == 256);
  }
}

static Task<void> stopped_accept(
  weave::Context &ctx,
  std::atomic<unsigned> &started,
  std::atomic<unsigned> &destroyed,
  std::atomic<unsigned> &unreachable)
{
  struct Count {
    std::atomic<unsigned> &value;

    ~Count()
    {
      ++value;
    }
  } guard{destroyed};

  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  if (!listener)
    co_await weave::fail(listener.error());
  started.fetch_add(1);
  started.notify_all();
  (void)co_await listener->accept();
  ++unreachable;
}

TEST_CASE("Runtime shutdown drains pending AcceptEx before destroying failed Tasks")
{
  for (auto mode : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = mode});
    REQUIRE(runtime);
    std::atomic<unsigned> started = 0, destroyed = 0, unreachable = 0;
    std::vector<weave::JoinHandle<void>> jobs;
    for (int i = 0; i < 64; ++i) {
      auto job = runtime->spawn([&](weave::Context &ctx) {
        return stopped_accept(ctx, started, destroyed, unreachable);
      });
      REQUIRE(job);
      jobs.push_back(std::move(*job));
    }
    for (auto n = started.load(); n != 64; n = started.load())
      started.wait(n);
    runtime->shutdown();
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::operation_canceled);
    }
    CHECK(destroyed == 64);
    CHECK(unreachable == 0);
  }
}

TEST_CASE("Ready and pending joins propagate failures and support explicit recovery")
{
  for (auto mode : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    for (bool ready_first : {false, true}) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = mode});
      REQUIRE(runtime);
      std::atomic<int> destroyed = 0, unreachable = 0;
      auto child = [&](weave::Context &ctx) -> Task<int> {
        struct Count {
          std::atomic<int> &value;
          ~Count()
          {
            ++value;
          }
        } guard{destroyed};
        co_await ctx.yield();
        co_await weave::fail(std::errc::io_error);
        ++unreachable;
        co_return 0;
      };
      auto parent = [&](weave::Context &ctx) -> Task<int> {
        auto job = runtime->spawn_on(0, child);
        if (!job)
          co_await weave::fail(job.error());
        if (ready_first) {
          while (!job->ready())
            co_await ctx.yield();
        }
        CHECK(job->ready() == ready_first);
        (void)co_await std::move(*job);
        ++unreachable;
        co_return 0;
      };
      auto root = runtime->spawn_on(0, parent);
      REQUIRE(root);
      auto result = std::move(*root).get();
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::io_error);
      CHECK(destroyed == 1);
      auto recovery = runtime->spawn_on(0, [&](weave::Context &) -> Task<int> {
        auto job = runtime->spawn_on(0, child);
        if (!job)
          co_await weave::fail(job.error());
        auto error = co_await weave::as_result(std::move(*job));
        CHECK_FALSE(error);
        CHECK(error.error() == std::errc::io_error);
        CHECK(destroyed == 2);
        co_return 42;
      });
      REQUIRE(recovery);
      CHECK(std::move(*recovery).get() == 42);
      CHECK(unreachable == 0);
      auto zero = runtime->spawn([](weave::Context &) -> Task<void> {
        co_await weave::fail(weave::Error{});
      });
      REQUIRE(zero);
      CHECK_FALSE(std::move(*zero).get());
      runtime->join();
    }
  }
}

} // namespace test_task
