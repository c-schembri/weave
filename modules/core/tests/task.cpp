#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/core.hpp>
#include <memory>
#include <vector>

namespace test_core {

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

} // namespace test_core
