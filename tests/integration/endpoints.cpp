#include <doctest/doctest.h>
#include <weave/runtime.hpp>
#include <weave/tcp.hpp>
#include <weave/resolve.hpp>
#include <weave/timer.hpp>
#include <array>
#include <atomic>

static weave::Task<void> resolved_client(weave::u16 port)
{
  auto endpoints = co_await weave::resolve("localhost", port, {.family = weave::AddressFamily::v6});
  auto stream = co_await weave::tcp::connect(std::move(endpoints));
  std::array<std::byte, 1025> sent{};
  sent.back() = std::byte{42};
  std::array<std::byte, 1025> received{};
  co_await stream.write_all(sent);
  co_await stream.read_exactly(received);
  if (sent != received)
    co_await weave::fail(std::errc::io_error);
  auto status = stream.shutdown_send();
  if (!status)
    co_await weave::fail(status.error());
  if (co_await stream.read(received) != 0)
    co_await weave::fail(std::errc::io_error);
}

static weave::Task<void> resolved_echo(weave::TcpStream stream, std::atomic<unsigned> &completed)
{
  std::array<std::byte, 97> buffer{};
  while (auto count = co_await stream.read(buffer))
    co_await stream.write_all(std::span(buffer).first(count));
  ++completed;
}

static weave::Task<void> resolved_burst(std::atomic<unsigned> &completed)
{
  constexpr unsigned count = 128;
  auto listener = co_await weave::tcp::listen(weave::Endpoint{weave::IpAddress::loopback_v6(), 0});
  co_await weave::scope([&](weave::TaskScope &scope) -> weave::Task<void> {
    std::vector<weave::JoinHandle<void>> clients;
    std::vector<weave::JoinHandle<void>> servers;
    for (unsigned i = 0; i < count; ++i) {
      auto job = scope.spawn(resolved_client(listener.local_port()));
      if (!job)
        co_await weave::fail(job.error());
      clients.push_back(std::move(*job));
    }
    for (unsigned i = 0; i < count; ++i) {
      auto stream = co_await listener.accept({.no_delay = true});
      auto job = scope.spawn(resolved_echo(std::move(stream), completed));
      if (!job)
        co_await weave::fail(job.error());
      servers.push_back(std::move(*job));
    }
    for (auto &job : clients)
      co_await std::move(job);
    for (auto &job : servers)
      co_await std::move(job);
  });
}

TEST_CASE("Concurrent IPv6 DNS connections work across runtime schedulers and IOCP layouts")
{
  constexpr std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  constexpr std::array layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
  constexpr std::array completion_modes{false, true};

  for (auto scheduler : schedulers) {
    for (auto layout : layouts) {
      for (bool skip : completion_modes) {
        CAPTURE(scheduler);
        CAPTURE(layout);
        CAPTURE(skip);
        std::atomic<unsigned> completed = 0;
        auto runtime = weave::Runtime::create(
          {.workers = 4,
            .scheduler = scheduler,
            .context = {.skip_successful_completions = skip},
            .io_layout = layout});
        REQUIRE(runtime);
        std::vector<weave::JoinHandle<void>> roots;
        for (unsigned worker = 0; worker < 4; ++worker) {
          auto root = runtime->spawn_on(worker, weave::timeout(std::chrono::seconds(10), resolved_burst(completed)));
          REQUIRE(root);
          roots.push_back(std::move(*root));
        }
        for (auto &root : roots) {
          auto result = std::move(root).get();
          CAPTURE(result ? 0 : result.error().value());
          CHECK(result);
        }
        CHECK(completed == 512);
      }
    }
  }
}
