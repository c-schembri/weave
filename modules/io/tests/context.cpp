#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/io.hpp>
#include "windows/iocp.hpp"
#include <weave/io/detail/context_access.hpp>
#include <type_traits>

static_assert(std::same_as<decltype(weave::Context::create()), weave::Result<weave::Context>>);
static_assert(noexcept(weave::Context::create()));
static_assert(!std::is_default_constructible_v<weave::Context>);
static_assert(!std::is_constructible_v<weave::Context, weave::ContextOptions>);
static_assert(!std::is_copy_constructible_v<weave::Context>);
static_assert(!std::is_move_constructible_v<weave::Context>);
static_assert(!std::is_move_assignable_v<weave::Context>);
static_assert(!std::is_move_constructible_v<weave::Result<weave::Context>>);

template <class T>
concept AcceptsEmptyFactoryKey = requires { T({}, {}); };

static_assert(!AcceptsEmptyFactoryKey<weave::Context>);

static HANDLE WINAPI fail_create_port(HANDLE file, HANDLE existing, ULONG_PTR key, DWORD concurrency)
{
  CHECK(file == INVALID_HANDLE_VALUE);
  CHECK(existing == nullptr);
  CHECK(key == 0);
  CHECK(concurrency == 1);
  SetLastError(ERROR_NOT_ENOUGH_QUOTA);
  return nullptr;
}

static weave::Result<weave::Context> make_context(weave::ContextOptions options)
{
  return weave::Context::create(options);
}

TEST_CASE("Context creation reports the native error without publishing a failed context")
{
  auto failed = weave::detail::IoAccess::create_context({}, &fail_create_port);
  REQUIRE_FALSE(failed);
  CHECK(failed.error() == weave::Error{ERROR_NOT_ENOUGH_QUOTA, std::system_category()});
  CHECK(weave::detail::current_context == nullptr);

  auto context = weave::Context::create();
  REQUIRE(context);
  CHECK(context->run(weave::when_all()));
}

TEST_CASE("Context factory preserves options and owns exactly one completion port")
{
  for (bool skip : {false, true}) {
    HANDLE port;
    {
      auto context = make_context({.skip_successful_completions = skip});
      REQUIRE(context);
      auto &state = weave::detail::IoAccess::state(*context);
      CHECK(state.options_.skip_successful_completions == skip);
      port = state.port_;
      DWORD flags;
      CHECK(GetHandleInformation(port, &flags) != FALSE);
      CHECK(context->run(weave::when_all()));
    }

    DWORD flags;
    auto valid = GetHandleInformation(port, &flags);
    auto error = GetLastError();
    CHECK(valid == FALSE);
    CHECK(error == ERROR_INVALID_HANDLE);
  }
}

TEST_CASE("IO context drives yielding tasks without TCP or runtime")
{
  auto context = weave::Context::create();
  REQUIRE(context);
  CHECK_FALSE(context->stop_requested());
  int calls = 0;
  auto operation = [&]() -> weave::Task<int> {
    for (int i = 0; i < 100; ++i) {
      co_await context->yield();
      ++calls;
    }
    co_return calls;
  };
  CHECK(context->run(operation()) == 100);
  CHECK(weave::detail::current_context == nullptr);
  CHECK(weave::detail::current_executor == nullptr);
  CHECK(context->metrics().dequeue_calls == 100);
}

TEST_CASE("IO context remains usable after asynchronous failure")
{
  auto context = weave::Context::create();
  REQUIRE(context);
  auto failed = [&]() -> weave::Task<void> {
    co_await context->yield();
    co_await weave::fail(std::errc::invalid_argument);
  };
  auto result = context->run(failed());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::invalid_argument);
  CHECK(weave::detail::current_context == nullptr);
  CHECK(context->run(weave::when_all()));
}

TEST_CASE("A shared I/O domain outlives its contexts and owns exactly one port")
{
  HANDLE port;
  {
    auto domain = weave::detail::ContextAccess::create_domain(2);
    REQUIRE(domain);
    port = (*domain)->port;
    {
      auto first = weave::detail::ContextAccess::create({}, *domain);
      auto second = weave::detail::ContextAccess::create({}, *domain);
      REQUIRE(first);
      REQUIRE(second);
      CHECK(weave::detail::IoAccess::state(*first).port_ == port);
      CHECK(weave::detail::IoAccess::state(*second).port_ == port);
      CHECK(first->run(weave::when_all()));
      CHECK(second->run(weave::when_all()));
    }
    DWORD flags;
    CHECK(GetHandleInformation(port, &flags) != FALSE);
  }
  DWORD flags;
  CHECK(GetHandleInformation(port, &flags) == FALSE);
  CHECK(GetLastError() == ERROR_INVALID_HANDLE);
}
