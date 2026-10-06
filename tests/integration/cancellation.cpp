#include <doctest/doctest.h>
#include <weave/runtime.hpp>
#include <weave/tcp.hpp>
#include <weave/timer.hpp>
#include <weave/scope.hpp>
#include "runtime_fixture.hpp"
#include <array>
#include <atomic>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static weave::Scheduler cancellation_scheduler()
{
  auto scheduler = weave::Scheduler::worker_affine;
  SUBCASE("worker-affine")
  {
    scheduler = weave::Scheduler::worker_affine;
  }
  SUBCASE("work-stealing")
  {
    scheduler = weave::Scheduler::work_stealing;
  }
  return scheduler;
}

TEST_CASE_TEMPLATE(
  "Timeout drains AcceptEx and a pending read before the socket is reused",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (bool skip : {false, true}) {
    auto runtime = support::create_runtime<Layout>(
      {.workers = 4, .scheduler = cancellation_scheduler(), .context = {.skip_successful_completions = skip}});
    REQUIRE(runtime);
    auto operation = []() -> weave::Task<void> {
      auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
      auto expired = co_await weave::as_result(weave::timeout(2ms, listener.accept()));
      REQUIRE_FALSE(expired);
      CHECK(expired.error() == std::errc::timed_out);

      auto peer = co_await weave::tcp::connect("127.0.0.1", listener.local_port());
      auto socket = co_await listener.accept();
      std::array<std::byte, 32> buffer{};
      auto read = co_await weave::as_result(weave::timeout(2ms, socket.read(buffer)));
      REQUIRE_FALSE(read);
      CHECK(read.error() == std::errc::timed_out);
      std::array payload{std::byte{7}, std::byte{8}, std::byte{9}};
      co_await peer.write_all(payload);
      CHECK(co_await socket.read(buffer) == payload.size());
      CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));
      co_await socket.write_all(payload);
      co_await peer.read_exactly(std::span{buffer}.first(payload.size()));
      CHECK(std::equal(payload.begin(), payload.end(), buffer.begin()));
      if (auto status = peer.shutdown_send(); !status)
        co_await weave::fail(status.error());
      CHECK(co_await socket.read(buffer) == 0);
    };
    CHECK(runtime->run(operation));
  }
}

TEST_CASE_TEMPLATE(
  "Cancelling an individual root drains native accept without stopping the runtime",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = cancellation_scheduler()});
  REQUIRE(runtime);
  std::atomic<bool> pending = false;
  std::atomic<int> destroyed = 0;
  auto operation = [&]() -> weave::Task<void> {
    struct Lifetime {
      std::atomic<int> &destroyed;
      ~Lifetime()
      {
        ++destroyed;
      }
    } lifetime{destroyed};
    auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
    pending.store(true, std::memory_order_release);
    pending.notify_one();
    co_await listener.accept();
    FAIL("Cancelled accept resumed the body");
  };
  auto job = runtime->spawn(operation());
  REQUIRE(job);
  pending.wait(false, std::memory_order_acquire);
  std::this_thread::sleep_for(2ms);
  job->cancel();
  auto result = std::move(*job).get();
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(destroyed == 1);
  CHECK_FALSE(runtime->stop_requested());
  CHECK(runtime->run(weave::sleep_for(1ms)));
}

TEST_CASE_TEMPLATE(
  "Per-operation cancellation does not cancel a sibling write on the same socket",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = cancellation_scheduler()});
  REQUIRE(runtime);
  auto operation = []() -> weave::Task<void> {
    auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
    auto peer = co_await weave::tcp::connect("127.0.0.1", listener.local_port());
    auto socket = co_await listener.accept();
    std::vector<std::byte> payload(2 * 1024 * 1024, std::byte{17});
    auto read = [&]() -> weave::Task<void> {
      std::array<std::byte, 1> buffer{};
      auto result = co_await weave::as_result(weave::timeout(1ms, socket.read(buffer)));
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::timed_out);
    };
    auto write = [&]() -> weave::Task<void> { co_await socket.write_all(payload); };
    auto receive = [&]() -> weave::Task<void> {
      co_await weave::sleep_for(5ms);
      std::vector<std::byte> buffer(payload.size());
      co_await peer.read_exactly(buffer);
      CHECK(buffer == payload);
    };
    co_await weave::when_all(read(), write(), receive());
  };
  CHECK(runtime->run(operation));
}

TEST_CASE_TEMPLATE(
  "Multicore cancellation and expiry races publish each timer outcome once",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = cancellation_scheduler()});
  REQUIRE(runtime);
  std::atomic<int> outcomes = 0, destroyed = 0;
  std::vector<std::thread> producers;
  for (int producer = 0; producer < 4; ++producer) {
    producers.emplace_back([&] {
      for (int n = 0; n < 128; ++n) {
        auto job = runtime->spawn([&]() -> weave::Task<void> {
          struct Lifetime {
            std::atomic<int> &destroyed;
            ~Lifetime()
            {
              ++destroyed;
            }
          } lifetime{destroyed};
          co_await weave::sleep_for(100us);
        });
        REQUIRE(job);
        if (n % 3 == 0)
          std::this_thread::yield();
        if (n % 3 != 2)
          job->cancel();
        auto result = std::move(*job).get();
        CHECK((result || result.error() == std::errc::operation_canceled));
        ++outcomes;
      }
    });
  }
  for (auto &producer : producers)
    producer.join();
  CHECK(outcomes == 512);
  CHECK(destroyed <= 512); // Precancellation is allowed to prevent entry.
  CHECK(runtime->run(weave::sleep_for(1ms)));
}

TEST_CASE_TEMPLATE(
  "A write timeout drains native buffer ownership before stream close",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = cancellation_scheduler()});
  REQUIRE(runtime);
  auto operation = []() -> weave::Task<void> {
    auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
    auto peer = co_await weave::tcp::connect("127.0.0.1", listener.local_port());
    auto socket = co_await listener.accept();
    std::vector<std::byte> buffer(32 * 1024 * 1024, std::byte{19});
    auto result = co_await weave::as_result(weave::timeout(2ms, socket.write_all(buffer)));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::timed_out);
    if (auto status = socket.close(); !status)
      co_await weave::fail(status.error());
  };
  CHECK(runtime->run(operation));
}

TEST_CASE_TEMPLATE(
  "A failed scope retains body-local TCP buffers until cancelled child I/O drains",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = cancellation_scheduler()});
  REQUIRE(runtime);
  std::atomic<int> reclaimed = 0, observed = 0;
  auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
    struct Lifetime {
      std::atomic<int> &reclaimed;
      ~Lifetime()
      {
        ++reclaimed;
      }
    } lifetime{reclaimed};
    auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
    auto peer = co_await weave::tcp::connect("127.0.0.1", listener.local_port());
    auto socket = co_await listener.accept();
    std::array<std::byte, 32> buffer{};
    std::atomic<bool> started = false;
    auto child = children.spawn([&]() -> weave::Task<void> {
      started.store(true, std::memory_order_release);
      auto result = co_await weave::as_result(socket.read(buffer));
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::operation_canceled);
      CHECK(reclaimed == 0);
      buffer[0] = std::byte{42};
      ++observed;
    });
    if (!child)
      co_await weave::fail(child.error());
    while (!started.load(std::memory_order_acquire))
      co_await weave::sleep_for(1ms);
    co_await weave::sleep_for(1ms);
    co_await weave::fail(std::errc::io_error);
  };
  auto result = runtime->run(weave::scope(body));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::io_error);
  CHECK(reclaimed == 1);
  CHECK(observed == 1);
}

TEST_CASE_TEMPLATE(
  "Scopes inherit runtime scheduling and drain concurrent borrowed child data",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = cancellation_scheduler()});
  REQUIRE(runtime);
  std::atomic<int> completed = 0;
  auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
    for (int n = 0; n < 64; ++n) {
      auto job = children.spawn([&]() -> weave::Task<void> {
        co_await weave::sleep_for(1ms);
        ++completed;
      });
      if (!job)
        co_await weave::fail(job.error());
    }
  };
  CHECK(runtime->run(weave::scope(body)));
  CHECK(completed == 64);
}

TEST_CASE_TEMPLATE(
  "Shutdown drains grouped detached timers under both schedulers and I/O layouts",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = cancellation_scheduler()});
  REQUIRE(runtime);
  weave::CancelSource stop;
  std::atomic<int> reported = 0;
  for (int n = 0; n < 128; ++n) {
    runtime->detach(weave::sleep_for(1h), {.cancel = stop.token()}, [&](weave::Error error) noexcept {
      CHECK(error == std::errc::operation_canceled);
      ++reported;
    });
  }
  stop.cancel();
  runtime->shutdown();
  CHECK(reported == 128);
}
