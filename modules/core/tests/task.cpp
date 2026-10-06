#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/core.hpp>
#include <memory>
#include <vector>

namespace test_core {

template <class T, class R = void>
concept TaskAwaitTransform = requires(typename weave::Task<R>::promise_type &promise, T &&value) {
  promise.await_transform(std::forward<T>(value));
};

TEST_CASE("Tasks reject synchronous Results in every value category")
{
  static_assert(!TaskAwaitTransform<weave::Result<void>>);
  static_assert(!TaskAwaitTransform<weave::Result<void> &>);
  static_assert(!TaskAwaitTransform<const weave::Result<void> &>);
  static_assert(!TaskAwaitTransform<const weave::Result<void>>);
  static_assert(!TaskAwaitTransform<weave::Result<int>>);
  static_assert(!TaskAwaitTransform<weave::Result<int> &>);
  static_assert(!TaskAwaitTransform<const weave::Result<int> &>);
  static_assert(!TaskAwaitTransform<weave::Result<std::unique_ptr<int>>>);
  static_assert(!TaskAwaitTransform<weave::Result<std::unique_ptr<int>> &>);
  static_assert(!TaskAwaitTransform<const weave::Result<std::unique_ptr<int>> &>);
  static_assert(!TaskAwaitTransform<weave::Result<int>, int>);
  static_assert(TaskAwaitTransform<weave::Task<int>>);
  static_assert(TaskAwaitTransform<decltype(weave::fail(std::errc::io_error))>);
  static_assert(TaskAwaitTransform<decltype(weave::cancellation_point())>);
}

template <class T>
static weave::Result<T> run(weave::Task<T> task)
{
  weave::detail::TaskAccess::start(task);
  REQUIRE(weave::detail::TaskAccess::done(task));
  return weave::detail::TaskAccess::take(task);
}

struct Guard {
  std::vector<int> &destroyed;
  int id;

  ~Guard()
  {
    destroyed.push_back(id);
  }
};

static weave::Task<int> child(std::vector<int> &destroyed, bool &continued)
{
  Guard guard{destroyed, 2};
  co_await weave::fail(std::errc::connection_reset);
  continued = true;
  co_return 0;
}

static weave::Task<int> parent(std::vector<int> &destroyed, bool &continued)
{
  Guard guard{destroyed, 1};
  co_return co_await child(destroyed, continued);
}

static weave::Task<std::unique_ptr<int>> value(int &calls)
{
  ++calls;
  co_return std::make_unique<int>(42);
}

TEST_CASE("Core tasks are lazy and support move-only values without IO")
{
  int calls = 0;
  auto task = value(calls);
  CHECK(calls == 0);
  auto result = run(std::move(task));
  REQUIRE(result);
  CHECK(**result == 42);
  CHECK(calls == 1);
}

TEST_CASE("Core failure skips the body and destroys children before parents")
{
  std::vector<int> destroyed;
  bool continued = false;
  auto result = run(parent(destroyed, continued));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::connection_reset);
  CHECK_FALSE(continued);
  CHECK(destroyed == std::vector<int>{2, 1});
}

TEST_CASE("Explicit failure preserves zero-valued errors and skips both void and value task bodies")
{
  for (auto error : {weave::Error{}, std::make_error_code(std::errc::io_error)}) {
    bool continued = false;
    auto operation = [&]() -> weave::Task<void> {
      co_await weave::fail(error);
      continued = true;
    };
    auto result = run(operation());
    REQUIRE_FALSE(result);
    CHECK(result.error() == error);
    CHECK_FALSE(continued);

    auto value_operation = [&]() -> weave::Task<int> {
      co_await weave::fail(error);
      continued = true;
      co_return 42;
    };
    auto value_result = run(value_operation());
    REQUIRE_FALSE(value_result);
    CHECK(value_result.error() == error);
    CHECK_FALSE(continued);
  }
}

TEST_CASE("Core recovery destroys a failed chain before continuing")
{
  std::vector<int> destroyed;
  bool continued = false;
  auto recover = [&]() -> weave::Task<int> {
    auto result = co_await weave::as_result(parent(destroyed, continued));
    CHECK_FALSE(result);
    CHECK(destroyed == std::vector<int>{2, 1});
    co_return 7;
  };
  CHECK(run(recover()) == 7);
  CHECK_FALSE(continued);
}

struct ManualResume {
  std::coroutine_handle<> &continuation;

  bool await_ready() const noexcept
  {
    return false;
  }

  void await_suspend(std::coroutine_handle<> handle) noexcept
  {
    continuation = handle;
  }

  void await_resume() const noexcept
  {
  }
};

TEST_CASE("Core tasks can suspend without an IO backend")
{
  std::coroutine_handle<> continuation;
  auto operation = [&]() -> weave::Task<void> {
    co_await ManualResume{continuation};
    co_await weave::fail(std::errc::operation_canceled);
  };
  auto task = operation();
  weave::detail::TaskAccess::start(task);
  CHECK_FALSE(weave::detail::TaskAccess::done(task));
  REQUIRE(continuation);
  continuation.resume();
  CHECK(weave::detail::TaskAccess::done(task));
  auto result = weave::detail::TaskAccess::take(task);
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
}

template <class T, class F>
concept CanObserveError = requires(T &&task, F &&observer) {
  std::forward<T>(task).on_error(std::forward<F>(observer));
};

using VoidObserver = decltype([](weave::Error) noexcept {});
using FunctionObserver = void (*)(weave::Error) noexcept;
using AsyncObserver = weave::Task<void> (*)(weave::Error) noexcept;
static_assert(CanObserveError<weave::Task<void>, VoidObserver>);
static_assert(CanObserveError<weave::Task<void>, FunctionObserver>);
static_assert(CanObserveError<weave::Task<int>, VoidObserver>);
static_assert(!CanObserveError<weave::Task<void> &, VoidObserver>);
static_assert(!CanObserveError<weave::Task<void>, decltype([](weave::Error) {})>);
static_assert(!CanObserveError<weave::Task<void>, decltype([](weave::Error) noexcept { return 1; })>);
static_assert(!CanObserveError<weave::Task<void>, AsyncObserver>);
static_assert(!CanObserveError<weave::Task<void>, decltype([](weave::Error &) noexcept {})>);

static weave::Task<void> owned_failure(std::unique_ptr<Guard> guard, int &started)
{
  ++started;
  co_await weave::fail(std::errc::io_error);
}

static weave::Task<int> returned_failure(weave::Error error)
{
  co_return std::unexpected(error);
}

TEST_CASE("Error observation is lazy and preserves void and move-only successes")
{
  int calls = 0, errors = 0;
  auto observer = [&](weave::Error) noexcept { ++errors; };
  auto task = value(calls).on_error(observer);
  CHECK(calls == 0);
  CHECK(errors == 0);
  auto result = run(std::move(task));
  REQUIRE(result);
  CHECK(**result == 42);
  CHECK(calls == 1);
  CHECK(run(weave::when_all().on_error(observer)));
  CHECK(errors == 0);
}

TEST_CASE("Chained error observers run inside-out after child cleanup without recovering")
{
  std::vector<int> destroyed, observed;
  bool continued = false;
  auto task = parent(destroyed, continued)
                .on_error([&](const weave::Error &error) noexcept {
                  CHECK(error == std::errc::connection_reset);
                  CHECK(destroyed == std::vector<int>{2, 1});
                  observed.push_back(1);
                })
                .on_error([&](weave::Error error) noexcept {
                  CHECK(error == std::errc::connection_reset);
                  observed.push_back(2);
                });
  auto caller = [&]() -> weave::Task<int> {
    auto result = co_await std::move(task);
    continued = true;
    co_return result;
  };
  auto result = run(caller());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::connection_reset);
  CHECK(observed == std::vector<int>{1, 2});
  CHECK_FALSE(continued);
}

TEST_CASE("Error observers preserve exact errors including a zero-valued unexpected result")
{
  for (auto error : {weave::Error{123, std::system_category()}, weave::Error{}}) {
    int calls = 0;
    auto recover = [&]() -> weave::Task<void> {
      auto result = co_await weave::as_result(returned_failure(error).on_error([&](weave::Error seen) noexcept {
        CHECK(seen == error);
        ++calls;
      }));
      REQUIRE_FALSE(result);
      CHECK(result.error() == error);
    };
    CHECK(run(recover()));
    CHECK(calls == 1);
  }
}

TEST_CASE("Error adapters own mutable move-only observers across suspension")
{
  std::vector<int> destroyed;
  std::coroutine_handle<> continuation;
  int calls = 0;
  auto operation = [&]() -> weave::Task<void> {
    co_await ManualResume{continuation};
    co_await weave::fail(std::errc::operation_canceled);
  };
  auto task = operation().on_error(
    [guard = std::make_unique<Guard>(destroyed, 1), &calls](weave::Error error) mutable noexcept {
      CHECK(error == std::errc::operation_canceled);
      REQUIRE(guard);
      ++calls;
      guard.reset();
    });
  weave::detail::TaskAccess::start(task);
  CHECK_FALSE(weave::detail::TaskAccess::done(task));
  CHECK(calls == 0);
  CHECK(destroyed.empty());
  REQUIRE(continuation);
  continuation.resume();
  CHECK(weave::detail::TaskAccess::done(task));
  auto result = weave::detail::TaskAccess::take(task);
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(calls == 1);
  CHECK(destroyed == std::vector<int>{1});
}

TEST_CASE("Dropping an unstarted error adapter destroys owned state without invoking it")
{
  std::vector<int> destroyed;
  int started = 0, calls = 0;
  {
    auto task = owned_failure(std::make_unique<Guard>(destroyed, 1), started)
                  .on_error(
                    [guard = std::make_unique<Guard>(destroyed, 2), &calls](weave::Error) noexcept { ++calls; });
    CHECK(started == 0);
    CHECK(destroyed.empty());
  }
  CHECK(started == 0);
  CHECK(calls == 0);
  CHECK(destroyed.size() == 2);
}

} // namespace test_core
