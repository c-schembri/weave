#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/sync.hpp>
#include <weave/io.hpp>
#include <weave/timer.hpp>
#include <memory>
#include <array>
#include <vector>
#include <thread>
#include <atomic>

using namespace std::chrono_literals;

static weave::Task<void> acquire(weave::Semaphore &semaphore, int id, std::vector<int> &order)
{
  auto permit = co_await semaphore.acquire();

  order.push_back(id);
}

TEST_CASE("Semaphore permits transfer ownership and release FIFO waiters")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  weave::Semaphore semaphore(1);
  auto held = semaphore.try_acquire();
  REQUIRE(held);
  CHECK_FALSE(semaphore.try_acquire());

  auto moved = std::move(*held);
  held->release();
  CHECK_FALSE(semaphore.try_acquire());

  std::vector<int> order;
  auto body = [&]() -> weave::Task<void> {
    std::vector<weave::JoinHandle<void>> jobs;
    for (int id = 0; id < 32; ++id) {
      auto job = ctx->spawn(acquire(semaphore, id, order));
      REQUIRE(job);
      jobs.push_back(std::move(*job));
    }

    co_await ctx->yield();
    CHECK(order.empty());

    moved.release();
    for (auto &job : jobs)
      co_await std::move(job);
  };

  CHECK(ctx->run(body()));
  REQUIRE(order.size() == 32);
  for (int id = 0; id < 32; ++id)
    CHECK(order[id] == id);

  CHECK(semaphore.try_acquire());

  semaphore.close();
  auto closed = semaphore.try_acquire();
  REQUIRE_FALSE(closed);
  CHECK(closed.error() == weave::SyncError::closed);
}

TEST_CASE("Semaphore cancellation and closure unlink suspended and pre-cancelled waiters")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  weave::Semaphore semaphore(1);
  auto held = semaphore.try_acquire();
  REQUIRE(held);

  std::vector<int> order;
  auto pre = weave::CancelSource{};
  pre.cancel();

  auto job = ctx->spawn(acquire(semaphore, 1, order), {.cancel = pre.token()});
  REQUIRE(job);
  auto cancelled = ctx->run(std::move(*job).as_task());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error() == std::errc::operation_canceled);

  const std::array<bool, 2> close_modes{false, true};
  for (bool close : close_modes) {
    weave::CancelSource source;
    auto waiting = ctx->spawn(acquire(semaphore, 2, order), {.cancel = source.token()});
    REQUIRE(waiting);

    auto cancel = [&]() -> weave::Task<void> {
      co_await ctx->yield();

      if (close)
        semaphore.close();
      else
        source.cancel();
    };

    auto stopper = ctx->spawn(cancel());
    REQUIRE(stopper);

    auto result = ctx->run(std::move(*waiting).as_task());
    REQUIRE_FALSE(result);
    CHECK(
      result.error() ==
      (close ? weave::make_error_code(weave::SyncError::closed) : std::make_error_code(std::errc::operation_canceled)));
    CHECK(ctx->run(std::move(*stopper).as_task()));
  }

  CHECK(order.empty());
  held->release();
}

TEST_CASE("Bounded channels preserve move-only values, backpressure, and drain on close")
{
  weave::Channel<std::unique_ptr<int>> channel(1);
  auto first = std::make_unique<int>(1);
  auto second = std::make_unique<int>(2);

  CHECK(channel.try_send(first));
  CHECK_FALSE(first);
  CHECK_FALSE(channel.try_send(second));
  REQUIRE(second);
  CHECK(*second == 2);

  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  bool sent = false;
  auto producer = [&]() -> weave::Task<void> {
    co_await channel.send(std::move(second));
    sent = true;
  };

  auto job = ctx->spawn(producer());
  REQUIRE(job);

  auto consume = [&]() -> weave::Task<void> {
    co_await ctx->yield();
    CHECK_FALSE(sent);

    auto value = co_await channel.receive();
    REQUIRE(value);
    CHECK(**value == 1);

    co_await std::move(*job);
    CHECK(sent);

    channel.close();
    value = co_await channel.receive();
    REQUIRE(value);
    CHECK(**value == 2);
    CHECK_FALSE(co_await channel.receive());
  };

  CHECK(ctx->run(consume()));

  auto closed = ctx->run(channel.send(std::make_unique<int>(3)));
  REQUIRE_FALSE(closed);
  CHECK(closed.error() == weave::SyncError::closed);
}

TEST_CASE("Cancelling send and receive leaves channels reusable and close wakes empty receivers")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  weave::Channel<int> channel(1);
  auto cancelled = ctx->run(weave::timeout(1ms, channel.receive()));
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error() == std::errc::timed_out);

  int one = 1;
  CHECK(channel.try_send(one));

  auto sender = ctx->run(weave::timeout(1ms, channel.send(2)));
  REQUIRE_FALSE(sender);
  CHECK(sender.error() == std::errc::timed_out);

  auto buffered = channel.try_receive();
  REQUIRE(buffered);
  REQUIRE(*buffered);
  CHECK(**buffered == 1);

  CHECK_FALSE(channel.try_receive());
  auto waiting = ctx->spawn(channel.receive());
  REQUIRE(waiting);

  auto close = [&]() -> weave::Task<void> {
    co_await ctx->yield();
    channel.close();
  };

  auto closer = ctx->spawn(close());
  REQUIRE(closer);

  auto ended = ctx->run(std::move(*waiting).as_task());
  REQUIRE(ended);
  CHECK_FALSE(*ended);

  CHECK(ctx->run(std::move(*closer).as_task()));
}

TEST_CASE("Independent Contexts communicate without migrating their continuations")
{
  weave::Channel<int> channel(3);
  weave::Semaphore semaphore(1);
  std::atomic<int> total = 0;

  std::thread producer([&] {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);

    auto body = [&]() -> weave::Task<void> {
      const auto thread = std::this_thread::get_id();

      for (int i = 0; i < 1000; ++i) {
        auto permit = co_await semaphore.acquire();
        co_await channel.send(i);
        CHECK(std::this_thread::get_id() == thread);
      }

      channel.close();
    };

    CHECK(ctx->run(body()));
  });

  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  auto body = [&]() -> weave::Task<void> {
    const auto thread = std::this_thread::get_id();
    int count = 0;

    while (auto value = co_await channel.receive()) {
      CHECK(*value == count++);
      CHECK(std::this_thread::get_id() == thread);
    }

    total = count;
  };

  CHECK(ctx->run(body()));
  producer.join();

  CHECK(total == 1000);
}

TEST_CASE("Context stop drains a channel waiter even with a shielded task token")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);

  weave::Channel<int> channel(1);
  auto body = [&]() -> weave::Task<void> {
    auto operation = channel.receive();
    weave::detail::TaskAccess::bind(operation, {});
    co_await std::move(operation);
  };

  auto job = ctx->spawn(body());
  REQUIRE(job);

  auto stop = [&]() -> weave::Task<void> {
    co_await ctx->yield();
    ctx->request_stop();
  };

  ctx->detach(stop());

  auto result = ctx->run(std::move(*job).as_task());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
}
