#include <doctest/doctest.h>
#include <weave/io.hpp>
#include <thread>

static weave::Task<int> cancellation_value(int &calls)
{
  ++calls;
  co_return 42;
}

TEST_CASE("Cancellation sources are shared, thread safe and idempotent")
{
  weave::CancelSource source;
  auto token = source.token();
  CHECK(token.stop_possible());
  CHECK_FALSE(token.stop_requested());
  auto copy = source;
  std::thread thread([&] { CHECK(copy.cancel()); });
  thread.join();
  CHECK(token.stop_requested());
  CHECK_FALSE(source.cancel());
  CHECK_FALSE(weave::CancelToken{}.stop_possible());
}

TEST_CASE("A cancelled join prevents an unstarted factory from executing")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int calls = 0, factories = 0;
  auto job = ctx->spawn([&] {
    ++factories;
    return cancellation_value(calls);
  });
  REQUIRE(job);
  job->cancel();
  auto result = ctx->run(std::move(*job).as_task());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(calls == 0);
  CHECK(factories == 0);
  CHECK(ctx->run(cancellation_value(calls)) == 42);
}

TEST_CASE("Precancellation on detached tasks cleans owned parameters before reporting")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  weave::CancelSource source;
  source.cancel();
  int destroyed = 0, reported = 0, calls = 0;

  struct Capture {
    int &destroyed;

    ~Capture()
    {
      ++destroyed;
    }
  };

  ctx->detach(
    [&, capture = std::make_unique<Capture>(destroyed)] { return cancellation_value(calls); },
    {.cancel = source.token()},
    [&](weave::Error error) noexcept {
      CHECK(error == std::errc::operation_canceled);
      CHECK(destroyed == 1);
      ++reported;
    });
  auto wait = [&]() -> weave::Task<void> {
    while (reported == 0)
      co_await ctx->yield();
  };
  REQUIRE(ctx->run(wait()));
  CHECK(reported == 1);
  CHECK(calls == 0);
}

TEST_CASE("Direct children inherit cancellation but independent detach does not")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  weave::CancelSource source;
  int child_calls = 0, independent_calls = 0;
  auto parent = [&]() -> weave::Task<void> {
    source.cancel();
    weave::detach(cancellation_value(independent_calls));
    auto child = co_await weave::as_result(cancellation_value(child_calls));
    CHECK_FALSE(child);
    CHECK(child.error() == std::errc::operation_canceled);
    co_await weave::cancellation_point();
    FAIL("Cancellation resumed the body");
  };
  auto job = ctx->spawn(parent(), {.cancel = source.token()});
  REQUIRE(job);
  auto result = ctx->run(std::move(*job).as_task());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  auto wait = [&]() -> weave::Task<void> {
    while (independent_calls == 0)
      co_await ctx->yield();
  };
  REQUIRE(ctx->run(wait()));
  CHECK(child_calls == 0);
  CHECK(independent_calls == 1);
}

TEST_CASE("when_all drains inherited cancellation without skipping its recovery boundary")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  weave::CancelSource source;
  int calls = 0;
  auto branch = [&]() -> weave::Task<void> {
    co_await ctx->yield();
    co_await weave::cancellation_point();
    ++calls;
  };
  auto parent = [&]() -> weave::Task<void> {
    auto cancel = [&]() -> weave::Task<void> {
      source.cancel();
      co_return;
    };
    auto result = co_await weave::as_result(weave::when_all(branch(), cancel()));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
  };
  auto job = ctx->spawn(parent(), {.cancel = source.token()});
  REQUIRE(job);
  CHECK(ctx->run(std::move(*job).as_task()));
  CHECK(calls == 0);
}
