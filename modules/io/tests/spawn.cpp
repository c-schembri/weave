#include <doctest/doctest.h>
#include <weave/io.hpp>
#include <memory>
#include <thread>
#include <vector>
#include "../../../test_support/context_fixture.hpp"

struct ContextCapture {
  int &destroyed;
  std::thread::id &thread;

  ~ContextCapture()
  {
    ++destroyed;
    thread = std::this_thread::get_id();
  }
};

static weave::Task<int> context_constant()
{
  co_return 42;
}

struct ContextOverloadedFactory {
  weave::Task<int> operator()(weave::Context &) const
  {
    return context_constant();
  }

  weave::Task<void> operator()() const
  {
    return weave::when_all();
  }
};

template <class F>
concept ContextSubmission = requires(weave::Context &ctx, F &&operation) {
  ctx.spawn(std::forward<F>(operation));
  ctx.detach(std::forward<F>(operation));
};

template <class F>
concept ScopedDetach = requires(F &&operation) { weave::detach(std::forward<F>(operation)); };

template <class H>
concept ScopedDetachHandler = requires(H handler) { weave::detach(context_constant(), std::move(handler)); };

TEST_CASE("Context accepts tasks and either factory signature with context-taking precedence")
{
  static_assert(ContextSubmission<weave::Task<int>>);
  static_assert(!ContextSubmission<weave::Task<int> &>);
  static_assert(!ContextSubmission<const weave::Task<int>>);
  static_assert(ContextSubmission<decltype(context_constant) &>);
  static_assert(!ContextSubmission<decltype([] { return 42; })>);
  static_assert(!ContextSubmission<decltype([] { return weave::Result<int>{42}; })>);
  static_assert(!ContextSubmission<weave::Task<int> &(*)()>);
  static_assert(!ContextSubmission < weave::Task<int> && (*)() >);
  static_assert(!ContextSubmission<decltype([](int) -> weave::Task<void> { co_return; })>);

  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto named = ctx->spawn(context_constant);
  REQUIRE(named);
  CHECK(ctx->run(std::move(*named).as_task()) == 42);
  auto overloaded = ctx->spawn(ContextOverloadedFactory{});
  static_assert(std::same_as<decltype(overloaded), weave::Result<weave::JoinHandle<int>>>);
  REQUIRE(overloaded);
  CHECK(ctx->run(std::move(*overloaded).as_task()) == 42);
  ctx->detach(context_constant);
  ctx->detach(ContextOverloadedFactory{});
  ctx->shutdown();
}

TEST_CASE("Context retains move-only nullary coroutine closures until completion")
{
  int destroyed = 0, calls = 0;
  std::thread::id cleanup;
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto factory = [&, capture = std::make_unique<ContextCapture>(destroyed, cleanup)]() -> weave::Task<int> {
    CHECK(destroyed == 0);
    ++calls;
    co_await ctx->yield();
    CHECK(capture != nullptr);
    co_return 42;
  };
  static_assert(ContextSubmission<decltype(factory)>);
  static_assert(!ContextSubmission<decltype(factory) &>);
  auto job = ctx->spawn(std::move(factory));
  REQUIRE(job);
  CHECK(calls == 0);
  CHECK(ctx->run(std::move(*job).as_task()) == 42);
  CHECK(calls == 1);
  CHECK(destroyed == 1);
  CHECK(cleanup == std::this_thread::get_id());
}

static weave::Task<std::unique_ptr<ContextCapture>> context_owned_task(
  weave::Context &ctx,
  std::unique_ptr<ContextCapture> capture,
  int &calls,
  bool delayed,
  bool fail)
{
  ++calls;
  if (delayed)
    co_await ctx.yield();
  if (fail)
    co_await weave::fail(std::errc::io_error);
  co_return std::move(capture);
}

TEST_CASE("Context consumes lazy tasks and owns their parameters through success and failure")
{
  for (bool detached : {false, true}) {
    for (bool delayed : {false, true}) {
      for (bool fail : {false, true}) {
        int calls = 0, destroyed = 0, reported = 0;
        std::thread::id cleanup;
        auto ctx = weave::Context::create();
        REQUIRE(ctx);
        auto operation = context_owned_task(
          *ctx,
          std::make_unique<ContextCapture>(destroyed, cleanup),
          calls,
          delayed,
          fail);
        CHECK(calls == 0);
        if (detached) {
          ctx->detach(std::move(operation), [&](weave::Error error) noexcept {
            CHECK(error == std::errc::io_error);
            CHECK(destroyed == 1);
            ++reported;
          });
          CHECK(calls == 0);
          CHECK(destroyed == 0);
        } else {
          auto job = ctx->spawn(std::move(operation));
          REQUIRE(job);
          CHECK(calls == 0);
          auto result = ctx->run(std::move(*job).as_task());
          if (fail) {
            REQUIRE_FALSE(result);
            CHECK(result.error() == std::errc::io_error);
          } else {
            REQUIRE(result);
            CHECK(destroyed == 0);
            result->reset();
          }
        }
        if (detached)
          REQUIRE(ctx->run(support::wait_context(*ctx, [&] { return destroyed == 1; })));
        ctx->shutdown();
        CHECK(calls == 1);
        CHECK(destroyed == 1);
        CHECK(reported == (detached && fail ? 1 : 0));
        CHECK(cleanup == std::this_thread::get_id());
      }
    }
  }
}

TEST_CASE("Rejected direct tasks reclaim unstarted frames on the submitter")
{
  int calls = 0, destroyed = 0, reported = 0;
  std::thread::id cleanup;
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  ctx->request_stop();
  std::thread submitter([&] {
    auto job = ctx->spawn(
      context_owned_task(*ctx, std::make_unique<ContextCapture>(destroyed, cleanup), calls, false, false));
    REQUIRE_FALSE(job);
    CHECK(job.error() == std::errc::operation_canceled);
    CHECK(destroyed == 1);
    ctx->detach(
      context_owned_task(*ctx, std::make_unique<ContextCapture>(destroyed, cleanup), calls, false, false),
      [&](weave::Error error) noexcept {
        CHECK(error == std::errc::operation_canceled);
        ++reported;
        ctx->detach(context_constant());
      });
    CHECK(destroyed == 2);
    CHECK(cleanup == std::this_thread::get_id());
  });
  submitter.join();
  CHECK(calls == 0);
  CHECK(reported == 1);
  int factories = 0;
  ctx->detach([&] {
    ++factories;
    return context_constant();
  });
  CHECK(factories == 0);
}

TEST_CASE("Context owns deferred factories and reclaims captures before publishing joins")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  const auto owner = std::this_thread::get_id();
  int calls = 0, destroyed = 0;
  std::thread::id cleanup;
  auto capture = std::unique_ptr<ContextCapture>{new ContextCapture{destroyed, cleanup}};
  auto job = ctx->spawn([&, capture = std::move(capture)](weave::Context &current) -> weave::Task<int> {
    CHECK(&current == &*ctx);
    CHECK(std::this_thread::get_id() == owner);
    ++calls;
    co_await current.yield();
    CHECK(std::this_thread::get_id() == owner);
    co_return 42;
  });
  REQUIRE(job);
  CHECK_FALSE(job->ready());
  CHECK(calls == 0);
  CHECK(destroyed == 0);
  CHECK(ctx->run(std::move(*job).as_task()) == 42);
  CHECK(calls == 1);
  CHECK(destroyed == 1);
  CHECK(cleanup == owner);
}

TEST_CASE("Context joins preserve immediate and suspended errors and move-only values")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  for (bool delayed : {false, true}) {
    auto job = ctx->spawn([delayed](weave::Context &current) -> weave::Task<int> {
      if (delayed)
        co_await current.yield();
      co_await weave::fail(std::errc::io_error);
      FAIL("A failed spawned task resumed");
      co_return 0;
    });
    REQUIRE(job);
    auto result = ctx->run(std::move(*job).as_task());
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::io_error);
    CHECK_FALSE(ctx->stop_requested());
  }

  auto parent = ctx->spawn([](weave::Context &current) -> weave::Task<std::unique_ptr<int>> {
    auto child = current.spawn([](weave::Context &child_ctx) -> weave::Task<std::unique_ptr<int>> {
      co_await child_ctx.yield();
      co_return std::make_unique<int>(73);
    });
    if (!child)
      co_await weave::fail(child.error());
    co_return co_await std::move(*child);
  });
  REQUIRE(parent);
  auto result = ctx->run(std::move(*parent).as_task());
  REQUIRE(result);
  CHECK(**result == 73);
}

TEST_CASE("Context run serves submissions until cooperative stop and drains queued roots")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int completed = 0;
  std::vector<weave::JoinHandle<int>> jobs;
  for (int i = 0; i < 128; ++i) {
    auto job = ctx->spawn([&, i](weave::Context &current) -> weave::Task<int> {
      co_await current.yield();
      if (++completed == 128)
        current.request_stop();
      co_return i;
    });
    REQUIRE(job);
    jobs.push_back(std::move(*job));
  }
  ctx->run();
  CHECK(completed == 128);
  CHECK(ctx->stop_requested());
  for (int i = 0; i < 128; ++i) {
    REQUIRE(jobs[i].ready());
    CHECK(std::move(jobs[i]).get() == i);
  }
  auto rejected = ctx->spawn([](weave::Context &) -> weave::Task<void> { co_return; });
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == std::errc::operation_canceled);
  ctx->shutdown();
  ctx->shutdown();
}

TEST_CASE("Discarding context handles does not cancel work before context shutdown")
{
  int completed = 0, destroyed = 0;
  std::thread::id cleanup;
  const auto owner = std::this_thread::get_id();
  {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    for (int i = 0; i < 64; ++i) {
      auto capture = std::unique_ptr<ContextCapture>{new ContextCapture{destroyed, cleanup}};
      auto job = ctx->spawn([&, capture = std::move(capture)](weave::Context &current) -> weave::Task<void> {
        co_await current.yield();
        ++completed;
      });
      REQUIRE(job);
    }
    CHECK(completed == 0);
    REQUIRE(ctx->run(support::wait_context(*ctx, [&] { return completed == 64 && destroyed == 64; })));
  }
  CHECK(completed == 64);
  CHECK(destroyed == 64);
  CHECK(cleanup == owner);
}

TEST_CASE("Empty contexts may be created and destroyed inside another context task")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto operation = []() -> weave::Task<void> {
    auto nested = weave::Context::create();
    REQUIRE(nested);
    co_return;
  };
  CHECK(ctx->run(operation()));
}

static weave::Task<void> observed_context_failure(weave::Context &ctx, bool delayed)
{
  if (delayed)
    co_await ctx.yield();
  co_await weave::fail(std::errc::io_error);
}

TEST_CASE("Context run observes errors without consuming them or preventing later runs")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int calls = 0;
  for (bool delayed : {false, true}) {
    auto result = ctx->run(observed_context_failure(*ctx, delayed).on_error([&](weave::Error error) noexcept {
      CHECK(error == std::errc::io_error);
      ++calls;
    }));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::io_error);
  }
  CHECK(calls == 2);
  CHECK(ctx->run(weave::when_all()));
}

TEST_CASE("Context retains error observers for joined and discarded handles")
{
  for (bool detached : {false, true}) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    const auto owner = std::this_thread::get_id();
    int calls = 0, destroyed = 0;
    std::thread::id cleanup;
    {
      auto job = ctx->spawn([&](weave::Context &current) {
        auto capture = std::unique_ptr<ContextCapture>{new ContextCapture{destroyed, cleanup}};
        return observed_context_failure(current, true)
          .on_error([&, capture = std::move(capture)](weave::Error error) noexcept {
            CHECK(error == std::errc::io_error);
            CHECK(std::this_thread::get_id() == owner);
            ++calls;
          });
      });
      REQUIRE(job);
      CHECK(calls == 0);
      if (!detached) {
        auto result = ctx->run(std::move(*job).as_task());
        REQUIRE_FALSE(result);
        CHECK(result.error() == std::errc::io_error);
        CHECK(destroyed == 1);
      }
    }
    REQUIRE(ctx->run(support::wait_context(*ctx, [&] { return destroyed == 1; })));
    ctx->shutdown();
    CHECK(calls == 1);
    CHECK(destroyed == 1);
    CHECK(cleanup == owner);

    auto task = observed_context_failure(*ctx, false).on_error([&](weave::Error) noexcept { ++calls; });
    auto rejected = ctx->spawn([task = std::move(task)](weave::Context &) mutable { return std::move(task); });
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == std::errc::operation_canceled);
    CHECK(calls == 1);
  }
}

TEST_CASE("Context detach owns move-only error handlers and drains immediate and suspended failures")
{
  const auto owner = std::this_thread::get_id();
  for (bool delayed : {false, true}) {
    for (auto error : {std::make_error_code(std::errc::io_error), weave::Error{}}) {
      int calls = 0, frames = 0, factories = 0, handlers = 0;
      std::thread::id cleanup;
      {
        auto ctx = weave::Context::create();
        REQUIRE(ctx);
        auto factory_capture = std::make_unique<ContextCapture>(factories, cleanup);
        auto handler_capture = std::make_unique<ContextCapture>(handlers, cleanup);
        ctx->detach(
          [&, capture = std::move(factory_capture)](weave::Context &current) -> weave::Task<void> {
            ContextCapture frame{frames, cleanup};
            if (delayed)
              co_await current.yield();
            co_await weave::fail(error);
            FAIL("Detached failure resumed its body");
          },
          [&, capture = std::move(handler_capture)](const weave::Error &reported) noexcept {
            CHECK(reported == error);
            CHECK(std::this_thread::get_id() == owner);
            CHECK(frames == 1);
            CHECK(factories == 0);
            CHECK(handlers == 0);
            ++calls;
          });
        CHECK(calls == 0);
        CHECK(frames == 0);
        CHECK(factories == 0);
        CHECK(handlers == 0);
        REQUIRE(ctx->run(support::wait_context(*ctx, [&] { return calls == 1 && handlers == 1; })));
      }
      CHECK(calls == 1);
      CHECK(frames == 1);
      CHECK(factories == 1);
      CHECK(handlers == 1);
      CHECK(cleanup == owner);
    }
  }
}

TEST_CASE("Rejected detach reports on the submitter without running the factory or holding admission locks")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  ctx->request_stop();
  int destroyed = 0, handlers = 0, invoked = 0, reported = 0;
  std::thread::id cleanup;
  std::thread submitter([&] {
    auto caller = std::this_thread::get_id();
    auto capture = std::make_unique<ContextCapture>(destroyed, cleanup);
    auto factory = [&, capture = std::move(capture)](weave::Context &current) {
      ++invoked;
      return observed_context_failure(current, false);
    };
    auto handler = [&, capture = std::make_unique<ContextCapture>(handlers, cleanup)](weave::Error error) noexcept {
      CHECK(error == std::errc::operation_canceled);
      CHECK(std::this_thread::get_id() == caller);
      ++reported;
      ctx->request_stop();
      ctx->detach([&](weave::Context &) -> weave::Task<void> {
        ++invoked;
        co_return;
      });
    };
    static_assert(std::same_as<decltype(ctx->detach(std::move(factory), std::move(handler))), void>);
    ctx->detach(std::move(factory), std::move(handler));
    CHECK(reported == 1);
    CHECK(destroyed == 1);
    CHECK(handlers == 1);
    CHECK(cleanup == caller);
  });
  submitter.join();
  CHECK(reported == 1);
  CHECK(invoked == 0);
  CHECK(destroyed == 1);
  CHECK(handlers == 1);
}

static weave::Task<void> detached_context_success(weave::Context &ctx)
{
  co_await ctx.yield();
}

template <class H>
concept ContextDetachHandler = requires(weave::Context &ctx, H handler) {
  ctx.detach(detached_context_success, std::move(handler));
};

TEST_CASE("Detach returns void, ignores unobserved failures and never calls error handlers on success")
{
  static_assert(ContextDetachHandler<decltype([](weave::Error) noexcept {})>);
  static_assert(!ContextDetachHandler<decltype([](weave::Error) {})>);
  static_assert(!ContextDetachHandler<decltype([](weave::Error) noexcept { return 1; })>);
  static_assert(!ContextDetachHandler<decltype([](weave::Error) noexcept -> weave::Task<void> { co_return; })>);

  int calls = 0, destroyed = 0;
  std::thread::id cleanup;
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  static_assert(std::same_as<decltype(ctx->detach(detached_context_success)), void>);
  auto capture = std::make_unique<ContextCapture>(destroyed, cleanup);
  ctx->detach(detached_context_success, [&, capture = std::move(capture)](weave::Error) noexcept { ++calls; });
  ctx->detach([](weave::Context &current) { return observed_context_failure(current, true); });
  REQUIRE(ctx->run(support::wait_context(*ctx, [&] { return destroyed == 1; })));
  ctx->shutdown();
  CHECK(calls == 0);
  CHECK(destroyed == 1);
  CHECK(cleanup == std::this_thread::get_id());

  bool continued = false;
  auto submit = [&]() -> weave::Task<void> {
    ctx->detach(detached_context_success, [&](weave::Error error) noexcept {
      CHECK(error == std::errc::operation_canceled);
      ++calls;
    });
    continued = true;
    co_return;
  };
  auto task = submit();
  weave::detail::TaskAccess::bind(task, {});
  CHECK(ctx->run(std::move(task)));
  CHECK(continued);
  CHECK(calls == 1);
}

TEST_CASE("Scoped detach accepts owned tasks and factories and drains on the standalone Context")
{
  static_assert(ScopedDetach<weave::Task<int>>);
  static_assert(!ScopedDetach<weave::Task<int> &>);
  static_assert(ScopedDetach<decltype(context_constant) &>);
  static_assert(ScopedDetach<ContextOverloadedFactory>);
  static_assert(!ScopedDetach<decltype([] { return 42; })>);
  static_assert(std::same_as<decltype(weave::detach(context_constant())), void>);
  static_assert(ScopedDetachHandler<decltype([](weave::Error) noexcept {})>);
  static_assert(!ScopedDetachHandler<decltype([](weave::Error) {})>);
  static_assert(!ScopedDetachHandler<decltype([](weave::Error) noexcept { return 1; })>);
  for (bool direct : {false, true}) {
    for (bool delayed : {false, true}) {
      for (bool fail : {false, true}) {
        int calls = 0, destroyed = 0, observed = 0, handlers = 0;
        std::thread::id cleanup;
        auto ctx = weave::Context::create();
        REQUIRE(ctx);
        auto parent = [&]() -> weave::Task<void> {
          auto on_error = [&, capture = std::make_unique<ContextCapture>(handlers, cleanup)](
                            weave::Error error) noexcept {
            CHECK(error == std::errc::io_error);
            CHECK(destroyed == 1);
            ++observed;
          };
          auto capture = std::make_unique<ContextCapture>(destroyed, cleanup);
          if (direct) {
            weave::detach(context_owned_task(*ctx, std::move(capture), calls, delayed, fail), std::move(on_error));
          } else {
            weave::detach(
              [&, capture = std::move(capture)](weave::Context &current) mutable {
                CHECK(&current == &*ctx);
                return context_owned_task(current, std::move(capture), calls, delayed, fail);
              },
              std::move(on_error));
          }
          co_return;
        };
        REQUIRE(ctx->run(parent()));
        CHECK(calls == 0);
        CHECK(weave::detail::current_context == nullptr);
        CHECK(weave::detail::current_submission == nullptr);
        REQUIRE(ctx->run(support::wait_context(*ctx, [&] { return handlers == 1; })));
        ctx->shutdown();
        CHECK(calls == 1);
        CHECK(destroyed == 1);
        CHECK(handlers == 1);
        CHECK(observed == (fail ? 1 : 0));
        CHECK(cleanup == std::this_thread::get_id());
      }
    }
  }
}

TEST_CASE("Scoped detach rejection is synchronous and reentrant without executing the factory")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  int invoked = 0, reported = 0;
  ctx->request_stop();
  auto parent = [&]() -> weave::Task<void> {
    weave::detach(
      [&] {
        ++invoked;
        return context_constant();
      },
      [&](weave::Error error) noexcept {
        CHECK(error == std::errc::operation_canceled);
        ++reported;
        weave::detach(context_constant());
        ctx->request_stop();
      });
    CHECK(reported == 1);
    co_return;
  };
  auto task = parent();
  weave::detail::TaskAccess::bind(task, {});
  REQUIRE(ctx->run(std::move(task)));
  CHECK(invoked == 0);
  CHECK(reported == 1);
  CHECK(weave::detail::current_submission == nullptr);
}

TEST_CASE("Two Contexts on one thread inherit separate scoped detach destinations")
{
  auto first = weave::Context::create();
  auto second = weave::Context::create();
  REQUIRE(first);
  REQUIRE(second);
  int first_calls = 0, second_calls = 0;
  auto parent = [&](weave::Context &expected, int &calls) -> weave::Task<void> {
    weave::detach([&expected, &calls](weave::Context &current) -> weave::Task<void> {
      CHECK(&current == &expected);
      co_await current.yield();
      ++calls;
    });
    co_return;
  };
  REQUIRE(first->run(parent(*first, first_calls)));
  REQUIRE(second->run(parent(*second, second_calls)));
  REQUIRE(second->run(support::wait_context(*second, [&] { return second_calls == 1; })));
  second->shutdown();
  CHECK(second_calls == 1);
  CHECK(first_calls == 0);
  REQUIRE(first->run(support::wait_context(*first, [&] { return first_calls == 1; })));
  first->shutdown();
  CHECK(first_calls == 1);
  CHECK(weave::detail::current_context == nullptr);
  CHECK(weave::detail::current_submission == nullptr);
}
