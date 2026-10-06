#include <doctest/doctest.h>
#include <weave/timer.hpp>
#include <weave/io.hpp>
#include <atomic>
#include <limits>
#include <thread>

using namespace std::chrono_literals;
using TimerClock = std::chrono::steady_clock;

static weave::Task<int> timed_value(std::chrono::milliseconds delay)
{
  co_await weave::sleep_for(delay);
  co_return 42;
}

TEST_CASE("Timers use steady deadlines and do not complete early")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto task = [&]() -> weave::Task<void> {
    const auto deadline = TimerClock::now() + 10ms;
    co_await weave::sleep_until(deadline);
    CHECK(TimerClock::now() >= deadline);
    co_await weave::sleep_for(0ms);
    co_await weave::sleep_for(-1ms);
    co_await weave::sleep_until(TimerClock::now() - 1h);
    co_await weave::sleep_for(std::chrono::duration<double>{0.0001});
  };
  CHECK(ctx->run(task()));
}

TEST_CASE("Relative timers are lazy and invalid durations fail without suspension")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto timer = weave::sleep_for(10ms);
  std::this_thread::sleep_for(15ms);
  const auto start = TimerClock::now();
  CHECK(ctx->run(std::move(timer)));
  CHECK(TimerClock::now() - start >= 10ms);
  auto invalid = ctx->run(weave::sleep_for(std::chrono::duration<double>{std::numeric_limits<double>::quiet_NaN()}));
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == std::errc::invalid_argument);
}

TEST_CASE("Cancelling a pending timer drains it without stopping the Context")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  weave::CancelSource stop;
  std::atomic<bool> entered = false;
  auto task = [&]() -> weave::Task<void> {
    entered.store(true, std::memory_order_release);
    entered.notify_one();
    co_await weave::sleep_for(1h);
    FAIL("Cancelled timer resumed");
  };
  auto job = ctx->spawn(task(), {.cancel = stop.token()});
  REQUIRE(job);
  std::thread canceller([&] {
    entered.wait(false, std::memory_order_acquire);
    std::this_thread::sleep_for(5ms);
    stop.cancel();
  });
  auto result = ctx->run(std::move(*job).as_task());
  canceller.join();
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(ctx->run(timed_value(1ms)) == 42);
}

TEST_CASE("Context stop cancels even a shielded timer and drains the queue")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  std::atomic<bool> entered = false;
  auto task = [&]() -> weave::Task<void> {
    auto timer = weave::sleep_for(1h);
    weave::detail::TaskAccess::bind(timer, {});
    entered.store(true, std::memory_order_release);
    entered.notify_one();
    co_await std::move(timer);
  };
  std::thread stopper([&] {
    entered.wait(false, std::memory_order_acquire);
    std::this_thread::sleep_for(5ms);
    ctx->request_stop();
  });
  auto result = ctx->run(task());
  stopper.join();
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
}

TEST_CASE("Timeout preserves values and errors and drains the losing timer")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  CHECK(ctx->run(weave::timeout(1s, timed_value(1ms))) == 42);
  auto result = ctx->run(weave::timeout(1ms, timed_value(1h)));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::timed_out);
  CHECK(ctx->run(weave::timeout_at(TimerClock::now() + 1s, timed_value(1ms))) == 42);
  auto failure = []() -> weave::Task<void> { co_await weave::fail(std::errc::io_error); };
  auto failed = ctx->run(weave::timeout(1h, failure()));
  REQUIRE_FALSE(failed);
  CHECK(failed.error() == std::errc::io_error);
  auto immediate = []() -> weave::Task<int> { co_return 7; };
  CHECK(ctx->run(weave::timeout(0ms, immediate())) == 7);
}

TEST_CASE("Timeout cancellation is recoverable and drains both branches")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  weave::CancelSource source;
  auto parent = [&]() -> weave::Task<void> {
    auto cancel = [&]() -> weave::Task<void> {
      co_await weave::sleep_for(1ms);
      source.cancel();
    };
    auto operation = [&]() -> weave::Task<void> {
      auto result = co_await weave::as_result(weave::timeout(1h, timed_value(1h)));
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::operation_canceled);
    };
    co_await weave::when_all(operation(), cancel());
  };
  auto job = ctx->spawn(parent(), {.cancel = source.token()});
  REQUIRE(job);
  CHECK(ctx->run(std::move(*job).as_task()));
}
