#include <doctest/doctest.h>
#include <weave/io.hpp>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include "../../../test_support/context_fixture.hpp"

// A minimal external coordinator: no Runtime, native headers, or detail APIs.
class ContextThread {
  std::mutex mutex_;
  weave::Context *context_ = nullptr;
  weave::Error error_;
  std::atomic<bool> ready_{false};
  std::thread thread_;

public:
  ContextThread()
      : thread_([this] {
          auto ctx = weave::Context::create();
          {
            std::lock_guard lock(mutex_);
            if (ctx)
              context_ = &*ctx;
            else
              error_ = ctx.error();
          }
          ready_.store(true, std::memory_order_release);
          ready_.notify_one();
          if (ctx) {
            ctx->run();
            // Exclude submissions before destroying the thread-owned context.
            std::lock_guard lock(mutex_);
            context_ = nullptr;
          }
        })
  {
    ready_.wait(false, std::memory_order_acquire);
  }

  ~ContextThread()
  {
    stop();
  }

  weave::Result<void> status() const noexcept
  {
    if (error_)
      return std::unexpected(error_);
    return {};
  }

  template <class F>
  auto spawn(F &&factory) -> decltype(std::declval<weave::Context &>().spawn(std::forward<F>(factory)))
  {
    std::lock_guard lock(mutex_);
    if (!context_)
      return std::unexpected(std::make_error_code(std::errc::operation_canceled));
    return context_->spawn(std::forward<F>(factory));
  }

  template <class F>
  void detach(F &&factory)
  {
    std::lock_guard lock(mutex_);
    if (context_)
      context_->detach(std::forward<F>(factory));
  }

  void request_stop()
  {
    std::lock_guard lock(mutex_);
    if (context_)
      context_->request_stop();
  }

  void stop()
  {
    request_stop();
    if (thread_.joinable())
      thread_.join();
  }
};

TEST_CASE("A custom runtime can submit and join across contexts using only public IO APIs")
{
  ContextThread first, second;
  REQUIRE(first.status());
  REQUIRE(second.status());
  const auto caller = std::this_thread::get_id();
  for (bool ready_first : {false, true}) {
    auto child = first.spawn([caller](weave::Context &ctx) -> weave::Task<int> {
      CHECK(std::this_thread::get_id() != caller);
      co_await ctx.yield();
      co_return 41;
    });
    REQUIRE(child);
    if (ready_first) {
      while (!child->ready())
        std::this_thread::yield();
    }
    auto parent = second.spawn([child = std::move(*child), caller](weave::Context &) mutable -> weave::Task<int> {
      CHECK(std::this_thread::get_id() != caller);
      auto owner = std::this_thread::get_id();
      auto value = co_await std::move(child);
      CHECK(std::this_thread::get_id() == owner);
      co_return value + 1;
    });
    REQUIRE(parent);
    CHECK(std::move(*parent).get() == 42);
  }
  first.stop();
  second.stop();
}

TEST_CASE("A custom runtime inherits scoped detach through its public Context driver")
{
  ContextThread worker;
  REQUIRE(worker.status());
  std::atomic<int> completed = 0;
  auto parent = worker.spawn([&](weave::Context &ctx) -> weave::Task<void> {
    weave::detach([&, expected = &ctx](weave::Context &current) -> weave::Task<void> {
      CHECK(&current == expected);
      co_await current.yield();
      ++completed;
    });
    co_return;
  });
  REQUIRE(parent);
  REQUIRE(std::move(*parent).get());
  while (completed.load() != 1)
    std::this_thread::yield();
  worker.stop();
  CHECK(completed == 1);
}

TEST_CASE("Concurrent external context submitters publish every accepted result once")
{
  ContextThread worker;
  REQUIRE(worker.status());
  std::atomic<int> calls = 0, errors = 0;
  std::vector<std::thread> producers;
  for (int i = 0; i < 4; ++i) {
    producers.emplace_back([&] {
      for (int j = 0; j < 128; ++j) {
        auto job = worker.spawn([&calls, j](weave::Context &ctx) -> weave::Task<int> {
          ++calls;
          co_await ctx.yield();
          co_return j;
        });
        if (!job || std::move(*job).get() != j)
          ++errors;
      }
    });
  }
  for (auto &producer : producers)
    producer.join();
  worker.stop();
  CHECK(calls == 512);
  CHECK(errors == 0);
}

TEST_CASE("Context stop racing external submissions drains accepted tasks and rejects the rest")
{
  ContextThread worker;
  REQUIRE(worker.status());
  std::atomic<int> accepted = 0, finished = 0, cancelled = 0, errors = 0;
  std::atomic<bool> started = false;
  std::vector<std::thread> producers;
  for (int i = 0; i < 4; ++i) {
    producers.emplace_back([&] {
      for (int j = 0; j < 256; ++j) {
        auto job = worker.spawn([&](weave::Context &ctx) -> weave::Task<void> {
          co_await ctx.yield();
          ++finished;
        });
        if (!job) {
          if (job.error() != std::errc::operation_canceled)
            ++errors;
          break;
        }
        ++accepted;
        started.store(true, std::memory_order_release);
        started.notify_one();
        auto result = std::move(*job).get();
        if (!result) {
          if (result.error() == std::errc::operation_canceled)
            ++cancelled;
          else
            ++errors;
        }
      }
    });
  }
  started.wait(false, std::memory_order_acquire);
  worker.request_stop();
  for (auto &producer : producers)
    producer.join();
  worker.stop();
  CHECK(accepted > 0);
  CHECK(accepted == finished + cancelled);
  CHECK(errors == 0);
  auto rejected = worker.spawn([](weave::Context &) -> weave::Task<void> { co_return; });
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == std::errc::operation_canceled);
}

struct ContextDetachedValue {
  int &destroyed;
  std::thread::id owner = std::this_thread::get_id();

  ~ContextDetachedValue()
  {
    CHECK(std::this_thread::get_id() == owner);
    ++destroyed;
  }
};

static weave::Task<void> external_context_task(
  weave::Context &ctx,
  std::unique_ptr<ContextDetachedValue> capture,
  std::atomic<bool> &release)
{
  while (!release.load(std::memory_order_acquire))
    co_await ctx.yield();
  CHECK(capture != nullptr);
}

TEST_CASE("Prebuilt context tasks may outlive the thread that allocated their frames")
{
  int destroyed = 0;
  std::thread::id owner_thread;
  std::atomic<bool> release = false;
  ContextThread worker;
  REQUIRE(worker.status());
  auto owner = worker.spawn([&](weave::Context &ctx) -> weave::Task<weave::Context *> {
    owner_thread = std::this_thread::get_id();
    co_return &ctx;
  });
  REQUIRE(owner);
  auto ctx = std::move(*owner).get();
  REQUIRE(ctx);
  std::optional<weave::JoinHandle<void>> joined;
  std::thread submitter([&] {
    auto job = worker.spawn(
      external_context_task(**ctx, std::make_unique<ContextDetachedValue>(destroyed, owner_thread), release));
    REQUIRE(job);
    joined.emplace(std::move(*job));
    worker.detach(
      external_context_task(**ctx, std::make_unique<ContextDetachedValue>(destroyed, owner_thread), release));
  });
  submitter.join();
  release.store(true, std::memory_order_release);
  REQUIRE(joined);
  CHECK(std::move(*joined).get());
  worker.stop();
  CHECK(destroyed == 2);
}

TEST_CASE("Custom runtimes can detach and reclaim fast move-only results on the context thread")
{
  int destroyed = 0;
  ContextThread worker;
  REQUIRE(worker.status());
  for (int i = 0; i < 512; ++i) {
    worker.detach([&, i](weave::Context &ctx) -> weave::Task<std::unique_ptr<ContextDetachedValue>> {
      if (i % 2)
        co_await ctx.yield();
      co_return std::make_unique<ContextDetachedValue>(destroyed);
    });
  }
  auto drained = worker.spawn(
    [&](weave::Context &ctx) { return support::wait_context(ctx, [&] { return destroyed == 512; }); });
  REQUIRE(drained);
  REQUIRE(std::move(*drained).get());
  worker.stop();
  CHECK(destroyed == 512);
  bool invoked = false;
  worker.detach([&](weave::Context &) -> weave::Task<void> {
    invoked = true;
    co_return;
  });
  CHECK_FALSE(invoked);
}

TEST_CASE("External detach observers run and are reclaimed on the context thread")
{
  int observed = 0, destroyed = 0;
  std::thread::id owner_thread;
  ContextThread worker;
  REQUIRE(worker.status());
  auto owner = worker.spawn([&](weave::Context &ctx) -> weave::Task<weave::Context *> {
    owner_thread = std::this_thread::get_id();
    co_return &ctx;
  });
  REQUIRE(owner);
  auto ctx = std::move(*owner).get();
  REQUIRE(ctx);
  const auto caller = std::this_thread::get_id();
  for (int i = 0; i < 256; ++i) {
    (*ctx)->detach(
      [&, i](weave::Context &ctx) -> weave::Task<void> {
        if (i % 2)
          co_await ctx.yield();
        co_await weave::fail(std::errc::io_error);
      },
      [&, capture = std::make_unique<ContextDetachedValue>(destroyed, owner_thread)](weave::Error error) noexcept {
        CHECK(error == std::errc::io_error);
        CHECK(std::this_thread::get_id() != caller);
        ++observed;
      });
  }
  // No submissions use the borrowed context after stopping its lifetime owner.
  auto drained = worker.spawn([&](weave::Context &ctx) {
    return support::wait_context(ctx, [&] { return observed == 256 && destroyed == 256; });
  });
  REQUIRE(drained);
  REQUIRE(std::move(*drained).get());
  worker.stop();
  CHECK(observed == 256);
  CHECK(destroyed == 256);
}
