#include <doctest/doctest.h>
#include <weave/scope.hpp>
#include <weave/timer.hpp>
#include <atomic>
#include <memory>
#include <thread>

using namespace std::chrono_literals;

static weave::Task<void> scope_increment(int &count)
{
  co_await weave::sleep_for(1ms);
  ++count;
}

TEST_CASE("A scope drains children even when their join handles are discarded")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int count = 0;
  auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
    for (int n = 0; n < 32; ++n) {
      auto job = children.spawn(scope_increment(count));
      REQUIRE(job);
    }
    CHECK(count == 0);
    co_return;
  };
  REQUIRE(ctx->run(weave::scope(body)));
  CHECK(count == 32);
}

TEST_CASE("Scoped children can also be explicitly joined")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int count = 0;
  auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
    auto first = children.spawn(scope_increment(count));
    if (!first)
      co_await weave::fail(first.error());
    auto second = children.spawn(scope_increment(count));
    if (!second)
      co_await weave::fail(second.error());
    co_await std::move(*first);
    co_await std::move(*second);
    CHECK(count == 2);
  };
  CHECK(ctx->run(weave::scope(body)));
}

struct ScopeLifetime {
  int &destroyed;

  ~ScopeLifetime()
  {
    ++destroyed;
  }
};

TEST_CASE("Failure cancels and drains children before reclaiming the failed body frame")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int destroyed = 0, observed = 0;
  auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
    ScopeLifetime local{destroyed};
    auto child = [&]() -> weave::Task<void> {
      auto result = co_await weave::as_result(weave::sleep_for(1h));
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::operation_canceled);
      CHECK(local.destroyed == 0);
      ++observed;
    };
    auto job = children.spawn(child());
    if (!job)
      co_await weave::fail(job.error());
    co_await weave::sleep_for(1ms);
    co_await weave::fail(std::errc::io_error);
  };
  auto result = ctx->run(weave::scope(body));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::io_error);
  CHECK(destroyed == 1);
  CHECK(observed == 1);
}

TEST_CASE("A scope propagates unjoined child failures after joining all children")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int count = 0;
  auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
    auto fail = []() -> weave::Task<void> { co_await weave::fail(std::errc::io_error); };
    auto first = children.spawn(fail);
    if (!first)
      co_await weave::fail(first.error());
    auto second = children.spawn(scope_increment(count));
    if (!second)
      co_await weave::fail(second.error());
  };
  auto result = ctx->run(weave::scope(body));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::io_error);
  CHECK(count == 1);
}

TEST_CASE("Scope failure state does not confuse a zero-valued error code with success")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto body = [](weave::TaskScope &children) -> weave::Task<void> {
    auto failure = []() -> weave::Task<void> { co_await weave::fail(weave::Error{}); };
    auto child = children.spawn(failure);
    if (!child)
      co_await weave::fail(child.error());
  };
  auto result = ctx->run(weave::scope(body));
  REQUIRE_FALSE(result);
  CHECK(result.error() == weave::Error{});
}

TEST_CASE("Cancelling a parent scope drains all children and retains the body closure")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  weave::CancelSource stop;
  int destroyed = 0, reclaimed = 0;
  std::atomic<bool> entered = false;
  auto body = [&, capture = std::make_unique<ScopeLifetime>(destroyed)](
                weave::TaskScope &children) -> weave::Task<void> {
    auto child = [&]() -> weave::Task<void> {
      ScopeLifetime local{reclaimed};
      co_await weave::sleep_for(1h);
    };
    for (int n = 0; n < 16; ++n) {
      auto job = children.spawn(child());
      if (!job)
        co_await weave::fail(job.error());
    }
    co_await weave::sleep_for(1ms);
    entered.store(true, std::memory_order_release);
    entered.notify_one();
    co_await weave::sleep_for(1h);
  };
  auto job = ctx->spawn(weave::scope(std::move(body)), {.cancel = stop.token()});
  REQUIRE(job);
  std::thread thread([&] {
    entered.wait(false, std::memory_order_acquire);
    stop.cancel();
  });
  auto result = ctx->run(std::move(*job).as_task());
  thread.join();
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(reclaimed == 16);
  CHECK(destroyed == 1);
}
