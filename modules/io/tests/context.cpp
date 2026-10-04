#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/io.hpp>

TEST_CASE("IO context drives yielding tasks without TCP or runtime")
{
  weave::Context context;
  REQUIRE(context.status());
  CHECK_FALSE(context.stop_requested());
  int calls = 0;
  auto operation = [&]() -> weave::Task<int> {
    for (int i = 0; i < 100; ++i) {
      co_await context.yield();
      ++calls;
    }
    co_return calls;
  };
  CHECK(context.run(operation()) == 100);
  CHECK(weave::detail::current_context == nullptr);
  CHECK(weave::detail::current_executor == nullptr);
  CHECK(context.metrics().dequeue_calls == 100);
}

TEST_CASE("IO context remains usable after asynchronous failure")
{
  weave::Context context;
  REQUIRE(context.status());
  auto failed = [&]() -> weave::Task<void> {
    co_await context.yield();
    co_await weave::fail(std::errc::invalid_argument);
  };
  auto result = context.run(failed());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::invalid_argument);
  CHECK(weave::detail::current_context == nullptr);
  CHECK(context.run(weave::when_all()));
}
