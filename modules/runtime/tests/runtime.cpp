#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/runtime.hpp>
#include <atomic>
#include <vector>

TEST_CASE("Runtime schedules and drains both modes without TCP")
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    weave::Runtime runtime({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime.status());
    std::atomic<int> completed = 0;
    std::vector<weave::JoinHandle<int>> handles;
    for (int i = 0; i < 128; ++i) {
      auto handle = runtime.spawn([&, i](weave::Context &context) -> weave::Task<int> {
        for (int n = 0; n < 8; ++n)
          co_await context.yield();
        ++completed;
        co_return i;
      });
      REQUIRE(handle);
      handles.push_back(std::move(*handle));
    }
    for (int i = 0; i < 128; ++i)
      CHECK(std::move(handles[i]).get() == i);
    runtime.join();
    CHECK(completed == 128);
  }
}

TEST_CASE("Runtime joins nested failures through IO executor routing")
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    weave::Runtime runtime({.workers = 2, .scheduler = scheduler});
    REQUIRE(runtime.status());
    auto outer = runtime.spawn([&](weave::Context &) -> weave::Task<void> {
      auto inner = co_await runtime.spawn([](weave::Context &context) -> weave::Task<void> {
        co_await context.yield();
        co_await weave::fail(std::errc::operation_canceled);
      });
      co_await std::move(inner);
      FAIL("A failed child must skip the parent's remaining body");
    });
    REQUIRE(outer);
    auto result = std::move(*outer).get();
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
    runtime.join();
  }
}
