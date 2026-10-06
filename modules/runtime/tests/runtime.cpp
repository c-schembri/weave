#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/runtime.hpp>
#include "runtime_fixture.hpp"
#include <atomic>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <algorithm>
#include <array>

TEST_CASE_TEMPLATE(
  "Runtime factory returns only usable immovable runtimes",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  static_assert(!std::is_default_constructible_v<weave::Runtime>);
  static_assert(!std::is_constructible_v<weave::Runtime, weave::RuntimeOptions>);
  static_assert(!std::is_copy_constructible_v<weave::Runtime>);
  static_assert(!std::is_move_constructible_v<weave::Runtime>);
  static_assert(!std::is_move_constructible_v<weave::Result<weave::Runtime>>);
  static_assert(std::same_as<decltype(weave::Runtime::create()), weave::Result<weave::Runtime>>);
  static_assert(noexcept(weave::Runtime::create()));

  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    for (std::size_t workers : {1, 4}) {
      auto runtime = support::create_runtime<Layout>({.workers = workers, .scheduler = scheduler});
      REQUIRE(runtime);
      CHECK(runtime->worker_count() == workers);
      CHECK(runtime->scheduler() == scheduler);
      CHECK(runtime->io_layout() == Layout::value);
      CHECK_FALSE(runtime->stop_requested());
    }
  }
  for (int scheduler : {-1, 99}) {
    auto runtime = support::create_runtime<Layout>(
      {.workers = 4, .scheduler = static_cast<weave::Scheduler>(scheduler)});
    REQUIRE_FALSE(runtime);
    CHECK(runtime.error() == std::errc::invalid_argument);
  }
  auto runtime = weave::Runtime::create();
  REQUIRE(runtime);
  CHECK(runtime->worker_count() >= 1);
  CHECK(runtime->scheduler() == weave::Scheduler::worker_affine);
  CHECK(runtime->io_layout() == weave::IoLayout::sharded);
  auto invalid_layout = weave::Runtime::create({.workers = 1, .io_layout = static_cast<weave::IoLayout>(99)});
  REQUIRE_FALSE(invalid_layout);
  CHECK(invalid_layout.error() == std::errc::invalid_argument);
}

TEST_CASE_TEMPLATE("Runtime schedules and drains both modes without TCP", Layout, support::ShardedIo, support::SharedIo)
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    std::atomic<int> completed = 0;
    std::vector<weave::JoinHandle<int>> handles;
    for (int i = 0; i < 128; ++i) {
      auto handle = runtime->spawn([&, i](weave::Context &context) -> weave::Task<int> {
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
    runtime->join();
    CHECK(completed == 128);
  }
}

TEST_CASE_TEMPLATE(
  "Runtime joins nested failures through IO executor routing",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    auto runtime = support::create_runtime<Layout>({.workers = 2, .scheduler = scheduler});
    REQUIRE(runtime);
    auto outer = runtime->spawn([&](weave::Context &) -> weave::Task<void> {
      auto inner = runtime->spawn([](weave::Context &context) -> weave::Task<void> {
        co_await context.yield();
        co_await weave::fail(std::errc::operation_canceled);
      });
      if (!inner)
        co_await weave::fail(inner.error());
      co_await std::move(*inner);
      FAIL("A failed child must skip the parent's remaining body");
    });
    REQUIRE(outer);
    auto result = std::move(*outer).get();
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
    runtime->join();
  }
}

static weave::Task<int> observed_runtime_failure(weave::Context &ctx, bool delayed)
{
  if (delayed)
    co_await ctx.yield();
  co_return std::unexpected(std::make_error_code(std::errc::io_error));
}

TEST_CASE_TEMPLATE(
  "Both runtime schedulers observe detached errors and preserve joined results",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    std::atomic<int> observed = 0;
    std::vector<weave::JoinHandle<int>> handles;
    for (int i = 0; i < 64; ++i) {
      auto job = runtime->spawn([&, i](weave::Context &ctx) {
        return observed_runtime_failure(ctx, i % 2 != 0).on_error([&](weave::Error error) noexcept {
          CHECK(error == std::errc::io_error);
          ++observed;
        });
      });
      REQUIRE(job);
      if (i % 4 < 2)
        handles.push_back(std::move(*job));
    }
    runtime->join();
    CHECK(observed == 64);
    for (auto &handle : handles) {
      auto result = std::move(handle).get();
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::io_error);
    }
  }
}

struct RuntimeDetachedValue {
  std::atomic<int> &destroyed;
  std::thread::id submitter;

  ~RuntimeDetachedValue()
  {
    CHECK(std::this_thread::get_id() != submitter);
    ++destroyed;
  }
};

static weave::Task<std::thread::id> runtime_thread()
{
  co_return std::this_thread::get_id();
}

static weave::Task<std::unique_ptr<RuntimeDetachedValue>> runtime_owned_value(
  std::unique_ptr<RuntimeDetachedValue> value)
{
  co_return std::move(value);
}

template <class F>
concept RuntimeSubmission = requires(weave::Runtime &runtime, F &&operation) {
  runtime.spawn(std::forward<F>(operation));
  runtime.spawn_on(0, std::forward<F>(operation));
  runtime.detach(std::forward<F>(operation));
  runtime.detach_on(0, std::forward<F>(operation));
};

TEST_CASE_TEMPLATE(
  "Runtime consumes tasks and retains nullary factories under both schedulers",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  static_assert(RuntimeSubmission<weave::Task<int>>);
  static_assert(!RuntimeSubmission<weave::Task<int> &>);
  static_assert(!RuntimeSubmission<const weave::Task<int>>);
  static_assert(RuntimeSubmission<decltype(runtime_thread) &>);
  static_assert(!RuntimeSubmission<decltype([] { return 42; })>);
  static_assert(!RuntimeSubmission<weave::Task<int> &(*)()>);

  const auto caller = std::this_thread::get_id();
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    std::atomic<int> destroyed = 0, observed = 0;
    auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    for (int i = 0; i < 128; ++i) {
      runtime->detach(runtime_owned_value(std::make_unique<RuntimeDetachedValue>(destroyed, caller)));
    }
    auto joined = runtime->spawn(runtime_thread());
    REQUIRE(joined);
    auto thread = std::move(*joined).get();
    REQUIRE(thread);
    CHECK(*thread != caller);

    auto factory = [capture = std::make_unique<RuntimeDetachedValue>(destroyed, caller)]() -> weave::Task<int> {
      CHECK(capture != nullptr);
      co_return 42;
    };
    static_assert(RuntimeSubmission<decltype(factory)>);
    static_assert(!RuntimeSubmission<decltype(factory) &>);
    auto nullary = runtime->spawn(std::move(factory));
    REQUIRE(nullary);
    CHECK(std::move(*nullary).get() == 42);
    runtime->detach(runtime_thread);

    for (std::size_t i = 0; i < runtime->worker_count(); ++i) {
      auto owner = runtime->spawn_on(i, [](weave::Context &ctx) -> weave::Task<weave::Context *> { co_return &ctx; });
      REQUIRE(owner);
      auto ctx = std::move(*owner).get();
      REQUIRE(ctx);
      auto pinned = runtime->spawn_on(i, runtime_thread());
      REQUIRE(pinned);
      auto id = std::move(*pinned).get();
      REQUIRE(id);
      auto named = runtime->spawn_on(i, runtime_thread);
      REQUIRE(named);
      CHECK(std::move(*named).get() == *id);
      auto failed = runtime->spawn_on(i, observed_runtime_failure(**ctx, true));
      REQUIRE(failed);
      auto result = std::move(*failed).get();
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::io_error);
      runtime->detach_on(i, observed_runtime_failure(**ctx, true), [&, id = *id](weave::Error error) noexcept {
        CHECK(error == std::errc::io_error);
        CHECK(std::this_thread::get_id() == id);
        ++observed;
      });
      runtime->detach_on(
        i,
        [&, id = *id, capture = std::make_unique<RuntimeDetachedValue>(destroyed, caller)]() -> weave::Task<void> {
          CHECK(capture != nullptr);
          CHECK(std::this_thread::get_id() == id);
          co_return;
        });
    }
    runtime->join();
    CHECK(destroyed == 133);
    CHECK(observed == 4);
  }
}

struct RuntimeUnstartedFrame {
  int &destroyed;
  std::thread::id owner = std::this_thread::get_id();

  ~RuntimeUnstartedFrame()
  {
    CHECK(std::this_thread::get_id() == owner);
    ++destroyed;
  }
};

static weave::Task<void> runtime_unstarted_task(std::unique_ptr<RuntimeUnstartedFrame> capture)
{
  CHECK(capture != nullptr);
  FAIL("Rejected task executed its body");
  co_return;
}

TEST_CASE_TEMPLATE(
  "Runtime destroys rejected prebuilt frames without executing them",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  int destroyed = 0, reported = 0;
  const auto caller = std::this_thread::get_id();
  auto runtime = support::create_runtime<Layout>({.workers = 1});
  REQUIRE(runtime);
  auto invalid = runtime->spawn_on(1, runtime_unstarted_task(std::make_unique<RuntimeUnstartedFrame>(destroyed)));
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == std::errc::invalid_argument);
  CHECK(destroyed == 1);
  runtime->detach_on(
    1,
    runtime_unstarted_task(std::make_unique<RuntimeUnstartedFrame>(destroyed)),
    [&](weave::Error error) noexcept {
      CHECK(error == std::errc::invalid_argument);
      CHECK(std::this_thread::get_id() == caller);
      ++reported;
      runtime->detach(runtime_thread());
    });
  CHECK(destroyed == 2);
  runtime->join();
  auto stopped = runtime->spawn(runtime_unstarted_task(std::make_unique<RuntimeUnstartedFrame>(destroyed)));
  REQUIRE_FALSE(stopped);
  CHECK(stopped.error() == std::errc::operation_canceled);
  CHECK(destroyed == 3);
  runtime->detach(
    runtime_unstarted_task(std::make_unique<RuntimeUnstartedFrame>(destroyed)),
    [&](weave::Error error) noexcept {
      CHECK(error == std::errc::operation_canceled);
      CHECK(std::this_thread::get_id() == caller);
      ++reported;
    });
  CHECK(destroyed == 4);

  CHECK(reported == 2);
}

TEST_CASE_TEMPLATE(
  "Runtime detach drains fast values and observed errors under both schedulers",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  const auto submitter = std::this_thread::get_id();
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    std::atomic<int> destroyed = 0, observed = 0, handlers = 0, pinned = 0, pinned_errors = 0;
    auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    for (int i = 0; i < 256; ++i) {
      runtime->detach(
        [&, i](weave::Context &ctx) -> weave::Task<std::unique_ptr<RuntimeDetachedValue>> {
          if (i % 2)
            co_await ctx.yield();
          co_return std::make_unique<RuntimeDetachedValue>(destroyed, submitter);
        },
        [capture = std::make_unique<RuntimeDetachedValue>(handlers, submitter)](
          weave::Error) noexcept { FAIL("Successful detached task reported an error"); });
      runtime->detach(
        [i](weave::Context &ctx) { return observed_runtime_failure(ctx, i % 2 != 0); },
        [&, capture = std::make_unique<RuntimeDetachedValue>(handlers, submitter)](weave::Error error) noexcept {
          CHECK(error == std::errc::io_error);
          CHECK(std::this_thread::get_id() != submitter);
          ++observed;
        });
    }
    for (std::size_t i = 0; i < runtime->worker_count(); ++i) {
      auto owner = runtime->spawn_on(i, [](weave::Context &) -> weave::Task<std::thread::id> {
        co_return std::this_thread::get_id();
      });
      REQUIRE(owner);
      auto thread = std::move(*owner).get();
      REQUIRE(thread);
      runtime->detach_on(i, [&, thread = *thread](weave::Context &ctx) -> weave::Task<void> {
        co_await ctx.yield();
        CHECK(std::this_thread::get_id() == thread);
        ++pinned;
      });
      runtime->detach_on(
        i,
        [](weave::Context &ctx) { return observed_runtime_failure(ctx, true); },
        [&, thread = *thread](weave::Error error) noexcept {
          CHECK(error == std::errc::io_error);
          CHECK(std::this_thread::get_id() == thread);
          ++pinned_errors;
        });
    }
    runtime->join();
    CHECK(destroyed == 256);
    CHECK(observed == 256);
    CHECK(handlers == 512);
    CHECK(pinned == 4);
    CHECK(pinned_errors == 4);
    int rejected = 0;
    auto noop = [](weave::Context &) -> weave::Task<void> { co_return; };
    static_assert(std::same_as<decltype(runtime->detach(noop)), void>);
    static_assert(std::same_as<decltype(runtime->detach_on(0, noop)), void>);
    runtime->detach(noop, [&](weave::Error error) noexcept {
      CHECK(error == std::errc::operation_canceled);
      CHECK(std::this_thread::get_id() == submitter);
      ++rejected;
    });
    CHECK(rejected == 1);
  }
}

TEST_CASE_TEMPLATE(
  "Runtime detach reports invalid targets and closed admission without executing factories",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  int invoked = 0, rejected = 0;
  std::atomic<int> nested = 0;
  const auto caller = std::this_thread::get_id();
  auto factory = [&](weave::Context &) {
    ++invoked;
    return weave::when_all();
  };
  auto runtime = support::create_runtime<Layout>({.workers = 1});
  REQUIRE(runtime);
  runtime->detach_on(runtime->worker_count(), factory, [&](weave::Error error) noexcept {
    CHECK(error == std::errc::invalid_argument);
    CHECK(std::this_thread::get_id() == caller);
    ++rejected;
    runtime->detach([&](weave::Context &) -> weave::Task<void> {
      ++nested;
      co_return;
    });
  });
  CHECK(rejected == 1);
  runtime->join();
  CHECK(nested == 1);

  runtime->detach(factory, [&](weave::Error error) noexcept {
    CHECK(error == std::errc::operation_canceled);
    CHECK(std::this_thread::get_id() == caller);
    ++rejected;
  });
  runtime->detach(factory);
  CHECK(rejected == 2);
  CHECK(invoked == 0);
}

static weave::Task<int> runtime_immediate_failure(std::atomic<int> &started)
{
  ++started;
  co_return std::unexpected(std::make_error_code(std::errc::io_error));
}

TEST_CASE_TEMPLATE(
  "Detached error callbacks account for tasks and factories racing runtime shutdown",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    std::atomic<int> submitted = 0, started = 0, failed = 0, rejected = 0, cancelled = 0;
    auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    std::array<std::thread::id, 4> workers;
    for (std::size_t i = 0; i < workers.size(); ++i) {
      auto job = runtime->spawn_on(i, runtime_thread());
      REQUIRE(job);
      auto worker = std::move(*job).get();
      REQUIRE(worker);
      workers[i] = *worker;
    }
    auto submit = [&] {
      auto caller = std::this_thread::get_id();
      auto on_error = [&, caller](weave::Error error) noexcept {
        auto thread = std::this_thread::get_id();
        if (error == std::errc::operation_canceled) {
          // Rejection is synchronous; accepted work can be cancelled on a worker before entry.
          if (thread == caller) {
            ++rejected;
          } else {
            CHECK(std::find(workers.begin(), workers.end(), thread) != workers.end());
            ++cancelled;
          }
        } else {
          CHECK(error == std::errc::io_error);
          CHECK(std::find(workers.begin(), workers.end(), thread) != workers.end());
          ++failed;
          failed.notify_one();
        }
      };
      if (submitted.fetch_add(1) % 2) {
        runtime->detach(runtime_immediate_failure(started), on_error);
      } else {
        runtime->detach(
          [&](weave::Context &ctx) {
            ++started;
            return observed_runtime_failure(ctx, true);
          },
          on_error);
      }
    };
    submit();
    failed.wait(0); // Establish one completed failure before racing admission against shutdown.
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) {
      producers.emplace_back([&] {
        for (int j = 0; j < 128; ++j)
          submit();
      });
    }
    runtime->request_stop();
    for (auto &producer : producers)
      producer.join();
    runtime->shutdown();
    CHECK(started > 0);
    CHECK(failed <= started);
    CHECK(started <= failed + cancelled);
    CHECK(failed + rejected + cancelled == 513);
  }
}

TEST_CASE_TEMPLATE(
  "Runtime run supports tasks and factories and preserves results without stopping workers",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  const auto caller = std::this_thread::get_id();
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    std::atomic<int> started = 0, destroyed = 0;
    auto runtime = support::create_runtime<Layout>({.workers = 2, .scheduler = scheduler});
    REQUIRE(runtime);
    auto direct = runtime->run(runtime_thread());
    REQUIRE(direct);
    CHECK(*direct != caller);
    auto named = runtime->run(runtime_thread);
    REQUIRE(named);
    CHECK(*named != caller);

    auto value = runtime->run([]() -> weave::Task<std::unique_ptr<int>> { co_return std::make_unique<int>(42); });
    REQUIRE(value);
    CHECK(**value == 42);
    auto completed = runtime->run(
      [capture = std::make_unique<RuntimeDetachedValue>(destroyed, caller)](weave::Context &ctx) -> weave::Task<void> {
        co_await ctx.yield();
        CHECK(capture != nullptr);
      });
    CHECK(completed);
    CHECK(destroyed == 1);

    auto failed = runtime->run(runtime_immediate_failure(started));
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == std::errc::io_error);
    CHECK(started == 1);
    for (bool delayed : {false, true}) {
      auto result = runtime->run([delayed](weave::Context &ctx) { return observed_runtime_failure(ctx, delayed); });
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::io_error);
    }
    CHECK_FALSE(runtime->stop_requested());
    CHECK(runtime->run(weave::when_all()));

    runtime->request_stop();
    auto rejected = runtime->run([&] {
      ++started;
      return runtime_thread();
    });
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == std::errc::operation_canceled);
    CHECK(started == 1);
    int frames = 0;
    auto unstarted = runtime->run(runtime_unstarted_task(std::make_unique<RuntimeUnstartedFrame>(frames)));
    REQUIRE_FALSE(unstarted);
    CHECK(unstarted.error() == std::errc::operation_canceled);
    CHECK(frames == 1);
  }
}

TEST_CASE_TEMPLATE(
  "Runtime run waits only for its root and leaves independent work owned by the runtime",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    std::atomic<bool> release = false;
    std::atomic<int> finished = 0;
    auto runtime = support::create_runtime<Layout>({.workers = 1, .scheduler = scheduler});
    REQUIRE(runtime);
    auto result = runtime->run([&](weave::Context &ctx) -> weave::Task<void> {
      runtime->detach([&](weave::Context &worker) -> weave::Task<void> {
        while (!release.load(std::memory_order_acquire) && !worker.stop_requested())
          co_await worker.yield();
        ++finished;
      });
      co_await ctx.yield();
    });
    CHECK(result);
    CHECK(finished == 0);
    CHECK(runtime->run(runtime_thread()));
    release.store(true, std::memory_order_release);
    runtime->join();
    CHECK(finished == 1);
  }
}

TEST_CASE_TEMPLATE(
  "Factory runtime destruction drains detached work without explicit shutdown",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  const auto caller = std::this_thread::get_id();
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    std::atomic<int> destroyed = 0;
    {
      auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = scheduler});
      REQUIRE(runtime);
      for (int i = 0; i < 128; ++i)
        runtime->detach(runtime_owned_value(std::make_unique<RuntimeDetachedValue>(destroyed, caller)));
    }
    CHECK(destroyed == 128);
  }
}

TEST_CASE_TEMPLATE(
  "Scoped detach owns move-only closures, handlers and results under both scheduling policies",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  const auto caller = std::this_thread::get_id();
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    std::atomic<int> completed = 0, values = 0, handlers = 0, observed = 0;
    auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    for (std::size_t worker = 0; worker < 4; ++worker) {
      auto parent = runtime->spawn_on(worker, [&](weave::Context &) -> weave::Task<void> {
        const auto owner = std::this_thread::get_id();
        for (int i = 0; i < 64; ++i) {
          const auto error = i % 4 == 1 ? weave::Error{} : std::make_error_code(std::errc::io_error);
          weave::detach(
            [&, i, error, owner, capture = std::make_unique<RuntimeDetachedValue>(values, caller)](
              weave::Context &current) mutable -> weave::Task<std::unique_ptr<RuntimeDetachedValue>> {
              for (int n = 0; n < 8; ++n) {
                if (scheduler == weave::Scheduler::worker_affine)
                  CHECK(std::this_thread::get_id() == owner);
                co_await current.yield();
              }
              ++completed;
              if (i % 2)
                co_await weave::fail(error);
              co_return std::move(capture);
            },
            [&, error, capture = std::make_unique<RuntimeDetachedValue>(handlers, caller)](
              weave::Error reported) noexcept {
              CHECK(reported == error);
              CHECK(std::this_thread::get_id() != caller);
              ++observed;
            });
        }
        co_return;
      });
      REQUIRE(parent);
      REQUIRE(std::move(*parent).get());
    }
    runtime->join();
    CHECK(completed == 256);
    CHECK(values == 256);
    CHECK(handlers == 256);
    CHECK(observed == 128);
    CHECK(weave::detail::current_submission == nullptr);
  }
}

TEST_CASE_TEMPLATE(
  "Scoped detach on a stealing worker creates independent stealable roots even from Context-owned tasks",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (bool local : {false, true}) {
    auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
    REQUIRE(runtime);
    std::mutex mutex;
    std::condition_variable wake;
    int completed = 0;
    auto submit = [&](weave::Context &) -> weave::Task<bool> {
      const auto owner = std::this_thread::get_id();
      for (int i = 0; i < 3; ++i) {
        weave::detach([&, owner](weave::Context &ctx) -> weave::Task<void> {
          CHECK(std::this_thread::get_id() != owner);
          co_await ctx.yield();
          {
            std::lock_guard lock(mutex);
            ++completed;
          }
          wake.notify_one();
        });
      }
      // Blocking this worker is test-only: children must be independently stolen to finish.
      std::unique_lock lock(mutex);
      const bool stolen = wake.wait_for(lock, std::chrono::seconds(3), [&] { return completed == 3; });
      co_return stolen;
    };
    auto parent = runtime->spawn_on(0, [&](weave::Context &ctx) -> weave::Task<bool> {
      if (local) {
        auto child = ctx.spawn(submit);
        if (!child)
          co_await weave::fail(child.error());
        co_return co_await std::move(*child);
      }
      co_return co_await submit(ctx);
    });
    REQUIRE(parent);
    CHECK(std::move(*parent).get() == true);
    runtime->join();
    CHECK(completed == 3);
  }
}

TEST_CASE_TEMPLATE(
  "Scoped detach after runtime stop rejects synchronously and releases unstarted frames",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    auto runtime = support::create_runtime<Layout>({.workers = 2, .scheduler = scheduler});
    REQUIRE(runtime);
    int destroyed = 0, reported = 0, invoked = 0;
    auto parent = runtime->spawn_on(0, [&](weave::Context &) -> weave::Task<void> {
      const auto owner = std::this_thread::get_id();
      runtime->request_stop();
      weave::detach(
        runtime_unstarted_task(std::make_unique<RuntimeUnstartedFrame>(destroyed)),
        [&](weave::Error error) noexcept {
          CHECK(error == std::errc::operation_canceled);
          CHECK(std::this_thread::get_id() == owner);
          ++reported;
          weave::detach(runtime_thread());
        });
      CHECK(reported == 1);
      CHECK(destroyed == 1);
      weave::detach([&] {
        ++invoked;
        return runtime_thread();
      });
      co_return;
    });
    REQUIRE(parent);
    REQUIRE(std::move(*parent).get());
    runtime->join();
    CHECK(reported == 1);
    CHECK(destroyed == 1);
    CHECK(invoked == 0);
  }
}

TEST_CASE_TEMPLATE(
  "Mixed pinned and movable queue bursts drain exactly once while their parent blocks",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
  REQUIRE(runtime);
  std::array<std::thread::id, 4> owners;
  for (std::size_t worker = 0; worker < owners.size(); ++worker) {
    auto job = runtime->spawn_on(worker, runtime_thread());
    REQUIRE(job);
    auto owner = std::move(*job).get();
    REQUIRE(owner);
    owners[worker] = *owner;
  }

  constexpr std::size_t count = 384;
  constexpr int rounds = 8;
  std::array<std::atomic<int>, count> executions{};
  std::atomic<int> violations = 0;
  std::mutex mutex;
  std::condition_variable wake;
  std::size_t completed = 0;

  for (int round = 0; round < rounds; ++round) {
    completed = 0;
    auto parent = runtime->spawn_on(0, [&](weave::Context &) -> weave::Task<bool> {
      for (std::size_t index = 0; index < count; ++index) {
        const bool pinned = index % 4 == 0;
        const auto destination = 1 + index % 3;
        auto operation = [&, index, pinned, destination](weave::Context &ctx) -> weave::Task<void> {
          for (int turn = 0; turn < 4; ++turn) {
            if (pinned && std::this_thread::get_id() != owners[destination])
              ++violations;
            co_await ctx.yield();
          }
          ++executions[index];
          {
            std::lock_guard lock(mutex);
            ++completed;
          }
          wake.notify_one();
        };
        auto on_error = [&](weave::Error) noexcept { ++violations; };
        if (pinned)
          runtime->detach_on(destination, std::move(operation), on_error);
        else
          runtime->detach(std::move(operation), on_error);
      }
      // Test-only blocking forces thieves to publish and service their transferred batches.
      std::unique_lock lock(mutex);
      co_return wake.wait_for(lock, std::chrono::seconds(3), [&] { return completed == count; });
    });
    REQUIRE(parent);
    REQUIRE(std::move(*parent).get() == true);
  }
  runtime->join();
  CHECK(violations == 0);
  for (const auto &executed : executions)
    CHECK(executed.load() == rounds);
}
