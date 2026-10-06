#include <doctest/doctest.h>
#include <weave/sync.hpp>
#include <weave/runtime.hpp>
#include <array>
#include <atomic>
#include <vector>
#include <thread>
#include "runtime_fixture.hpp"

static weave::Task<void> messages(weave::Channel<int> &channel, int start)
{
  for (int i = 0; i < 256; ++i)
    co_await channel.send(start + i);
}

static weave::Task<void> collect(weave::Channel<int> &channel, std::array<std::atomic<int>, 1024> &counts)
{
  while (auto value = co_await channel.receive()) {
    REQUIRE(*value >= 0);
    REQUIRE(*value < static_cast<int>(counts.size()));
    ++counts[*value];
  }
}

static weave::Task<void> permit(weave::Semaphore &semaphore)
{
  auto held = co_await semaphore.acquire();
}

TEST_CASE("Channels and semaphore cancellation races drain across both runtime layouts and schedulers")
{
  constexpr auto layouts = support::io_layouts;
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};

  for (auto layout : layouts) {
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      REQUIRE(runtime);

      weave::Channel<int> channel(7);
      std::array<std::atomic<int>, 1024> counts{};
      std::vector<weave::JoinHandle<void>> receivers;
      std::vector<weave::JoinHandle<void>> senders;

      for (int i = 0; i < 4; ++i) {
        auto receiver = runtime->spawn(collect(channel, counts));
        auto sender = runtime->spawn(messages(channel, i * 256));
        REQUIRE(receiver);
        REQUIRE(sender);

        receivers.push_back(std::move(*receiver));
        senders.push_back(std::move(*sender));
      }

      for (auto &sender : senders)
        CHECK(std::move(sender).get());

      channel.close();

      for (auto &receiver : receivers)
        CHECK(std::move(receiver).get());

      for (const auto &count : counts)
        CHECK(count == 1);

      weave::Semaphore semaphore(1);
      for (int i = 0; i < 128; ++i) {
        auto held = semaphore.try_acquire();
        REQUIRE(held);

        weave::CancelSource source;
        auto job = runtime->spawn(permit(semaphore), {.cancel = source.token()});
        REQUIRE(job);

        std::thread canceller([&] {
          source.cancel();
        });
        held->release();
        auto result = std::move(*job).get();
        canceller.join();

        CHECK((result.has_value() || result.error() == std::errc::operation_canceled));
        CHECK(semaphore.try_acquire());
      }
    }
  }
}
