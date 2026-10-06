#include <doctest/doctest.h>
#include <weave/tcp.hpp>
#include <weave/timer.hpp>
#include "runtime_fixture.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>

using namespace std::chrono_literals;

struct ServeState {
  std::atomic<int> entered = 0;
  std::atomic<int> active = 0;
  std::atomic<int> destroyed = 0;
  std::atomic<int> handler_destroyed = 0;
  std::atomic<int> completed = 0;
  std::atomic<int> failed = 0;
  std::atomic<int> cancelled = 0;
};

struct ServeHandlerLifetime {
  ServeState &state;

  ~ServeHandlerLifetime()
  {
    CHECK(state.active == 0);
    ++state.handler_destroyed;
  }
};

struct ServeClientLifetime {
  ServeState &state;

  explicit ServeClientLifetime(ServeState &state) : state(state)
  {
    ++state.active;
    ++state.entered;
  }

  ~ServeClientLifetime()
  {
    CHECK(state.handler_destroyed == 0);
    --state.active;
    ++state.destroyed;
  }
};

struct ServeEchoHandler {
  ServeState &state;
  std::unique_ptr<ServeHandlerLifetime> lifetime;

  weave::Task<void> operator()(weave::TcpStream client)
  {
    ServeClientLifetime active{state};
    std::array<std::byte, 4096> buffer;
    for (;;) {
      auto received = co_await client.read(buffer);
      if (received == 0)
        co_return;
      if (buffer[0] == std::byte{255})
        co_await weave::fail(std::errc::io_error);
      co_await client.write_all(std::span{buffer}.first(received));
    }
  }
};

static weave::Task<void> serve_until_cancelled(weave::TcpListener &listener, ServeState &state)
{
  auto result = co_await weave::as_result(
    weave::tcp::serve(
      listener,
      {.no_delay = true},
      ServeEchoHandler{state, std::make_unique<ServeHandlerLifetime>(state)},
      [&](weave::Error error) noexcept {
        if (error == std::errc::operation_canceled)
          ++state.cancelled;
        else {
          CHECK(error == std::errc::io_error);
          ++state.failed;
        }
      }));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(state.active == 0);
  CHECK(state.handler_destroyed == 1);
}

static weave::Task<void> serve_roundtrip(weave::u16 port, int index, ServeState &state)
{
  auto client = co_await weave::tcp::connect("127.0.0.1", port);
  bool failing = index % 16 == 0;
  std::array<std::byte, 8193> sent{}, received{};
  for (std::size_t i = 0; i < sent.size(); ++i)
    sent[i] = static_cast<std::byte>(i % 251);
  if (failing)
    sent[0] = std::byte{255};
  co_await client.write_all(failing ? std::span{sent}.first(1) : std::span{sent});
  if (auto status = client.shutdown_send(); !status)
    co_await weave::fail(status.error());
  if (failing) {
    // Closing with unread input may produce either EOF or a reset on Windows.
    auto result = co_await weave::as_result(client.read(received));
    CHECK((!result || *result == 0));
  } else {
    co_await client.read_exactly(received);
    CHECK(sent == received);
    CHECK(co_await client.read(std::span{received}.first(1)) == 0);
  }
  ++state.completed;
}

TEST_CASE_TEMPLATE(
  "TCP serve inherits runtime scheduling and drains multicore clients before returning",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    for (bool skip : {false, true}) {
      CAPTURE(scheduler);
      CAPTURE(skip);
      auto runtime = support::create_runtime<Layout>(
        {.workers = 4, .scheduler = scheduler, .context = {.skip_successful_completions = skip}});
      REQUIRE(runtime);
      ServeState state;
      auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
        auto listener = co_await weave::tcp::listen("127.0.0.1", 0, 512);
        auto server = children.spawn(serve_until_cancelled(listener, state));
        if (!server)
          co_await weave::fail(server.error());

        auto idle = co_await weave::tcp::connect("127.0.0.1", listener.local_port());
        std::vector<weave::JoinHandle<void>> clients;
        for (int i = 0; i < 64; ++i) {
          auto client = children.spawn(serve_roundtrip(listener.local_port(), i, state));
          if (!client)
            co_await weave::fail(client.error());
          clients.push_back(std::move(*client));
        }
        for (auto &client : clients)
          co_await std::move(client);
        while (state.entered != 65)
          co_await weave::sleep_for(1ms);

        CHECK(state.completed == 64);
        server->cancel();
        co_await std::move(*server);
        CHECK(state.destroyed == 65);
        CHECK(state.failed == 4);
        CHECK(state.cancelled == 1);
      };
      REQUIRE(runtime->run(weave::timeout(5s, weave::scope(body))));
      CHECK_FALSE(runtime->stop_requested());
      REQUIRE(runtime->run(weave::sleep_for(1ms)));
    }
  }
}

static weave::Task<void> burst_client(
  weave::u16 port,
  std::size_t index,
  std::atomic<unsigned> &warmed,
  std::atomic<bool> &release,
  ServeState &state)
{
  auto client = co_await weave::tcp::connect("127.0.0.1", port);
  if (auto status = client.no_delay(); !status)
    co_await weave::fail(status.error());
  std::array<std::byte, 1024> sent{}, received{};
  for (std::size_t i = 0; i < sent.size(); ++i)
    sent[i] = static_cast<std::byte>((index * 31 + i * 17) % 251);

  for (unsigned round = 0; round < 8; ++round) {
    co_await client.write_all(sent);
    co_await client.read_exactly(received);
    if (sent != received)
      co_await weave::fail(std::errc::bad_message);
  }
  ++warmed;
  while (!release.load())
    co_await weave::sleep_for(1ms);

  co_await client.write_all(sent);
  if (auto status = client.shutdown_send(); !status)
    co_await weave::fail(status.error());
  co_await client.read_exactly(received);
  if (sent != received || co_await client.read(received) != 0)
    co_await weave::fail(std::errc::bad_message);
  ++state.completed;
}

TEST_CASE_TEMPLATE(
  "A single listener warms up 1024 live clients and drains their disconnect burst",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  constexpr unsigned count = 1024;
  for (bool skip : {false, true}) {
    CAPTURE(skip);
    ServeState state;
    std::atomic<unsigned> warmed = 0;
    std::atomic<bool> release = false;
    std::mutex failure_mutex;
    weave::Error first_failure;
    auto observe_failure = [&](weave::Error error) noexcept {
      if (error == std::errc::operation_canceled)
        return;
      std::lock_guard lock(failure_mutex);
      if (!first_failure)
        first_failure = error;
    };
    auto client_status = [&]() -> weave::Result<void> {
      std::lock_guard lock(failure_mutex);
      if (first_failure)
        return std::unexpected(first_failure);
      return {};
    };
    auto runtime = support::create_runtime<Layout>(
      {.workers = 4, .scheduler = weave::Scheduler::work_stealing, .context = {.skip_successful_completions = skip}});
    REQUIRE(runtime);

    auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
      auto listener = co_await weave::tcp::listen("127.0.0.1", 0, 8192);
      auto server = children.spawn(serve_until_cancelled(listener, state));
      if (!server)
        co_await weave::fail(server.error());
      std::vector<weave::JoinHandle<void>> clients;
      clients.reserve(count);
      for (unsigned i = 0; i < count; ++i) {
        auto client = children.spawn(
          burst_client(listener.local_port(), i, warmed, release, state).on_error(observe_failure));
        if (!client)
          co_await weave::fail(client.error());
        clients.push_back(std::move(*client));
      }
      while (warmed != count) {
        if (auto status = client_status(); !status)
          co_await weave::fail(status.error());
        if (server->ready())
          co_await weave::fail(std::errc::io_error);
        co_await weave::sleep_for(1ms);
      }
      CHECK(state.entered == count);
      CHECK(state.active == count);
      release = true;
      for (auto &client : clients)
        co_await std::move(client);

      server->cancel();
      co_await std::move(*server);
      CHECK(state.completed == count);
      CHECK(state.destroyed == count);
      CHECK(state.active == 0);
      CHECK(state.failed == 0);
      CHECK(state.cancelled == 0);
      CHECK(state.handler_destroyed == 1);
    };
    auto result = runtime->run(weave::timeout(10s, weave::scope(body)));
    CAPTURE(warmed.load());
    CAPTURE(state.completed.load());
    CAPTURE(state.entered.load());
    CAPTURE(result ? 0 : result.error().value());
    REQUIRE(result);
    REQUIRE(runtime->run(weave::sleep_for(1ms)));
  }
}

struct ServeDataBorrow {
  std::span<const std::byte> data;
  std::vector<std::byte> copy;

  explicit ServeDataBorrow(std::span<const std::byte> data) : data(data), copy(data.begin(), data.end())
  {
  }

  ~ServeDataBorrow()
  {
    CHECK(std::equal(data.begin(), data.end(), copy.begin(), copy.end()));
  }
};

struct ServeDataEchoHandler {
  ServeState &state;
  std::unique_ptr<ServeHandlerLifetime> lifetime;

  weave::Task<void> operator()(weave::TcpStream &client, std::span<const std::byte> data)
  {
    ServeClientLifetime active{state};
    ServeDataBorrow borrowed{data};
    CHECK_FALSE(data.empty());
    CHECK(data.size() <= 4096);
    if (data.front() == std::byte{254}) {
      co_await weave::sleep_for(1h);
      FAIL("Cancelled multicore data callback resumed its body");
    }
    if (data.front() == std::byte{255})
      co_await weave::fail(std::errc::io_error);
    co_await weave::sleep_for(1ms);
    co_await client.write_all(data);
  }
};

static weave::Task<void> serve_data_until_cancelled(weave::TcpListener &listener, ServeState &state)
{
  auto result = co_await weave::as_result(
    weave::tcp::serve(
      listener,
      {.no_delay = true},
      weave::tcp::on_data(ServeDataEchoHandler{state, std::make_unique<ServeHandlerLifetime>(state)}),
      [&](weave::Error error) noexcept {
        if (error == std::errc::operation_canceled)
          ++state.cancelled;
        else {
          CHECK(error == std::errc::io_error);
          ++state.failed;
        }
      }));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(state.active == 0);
  CHECK(state.handler_destroyed == 1);
}

TEST_CASE_TEMPLATE(
  "TCP on_data keeps borrowed buffers alive across multicore callbacks and cancellation",
  Layout,
  support::ShardedIo,
  support::SharedIo)
{
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    for (bool skip : {false, true}) {
      CAPTURE(scheduler);
      CAPTURE(skip);
      auto runtime = support::create_runtime<Layout>(
        {.workers = 4, .scheduler = scheduler, .context = {.skip_successful_completions = skip}});
      REQUIRE(runtime);
      ServeState state;
      auto body = [&](weave::TaskScope &children) -> weave::Task<void> {
        auto listener = co_await weave::tcp::listen("127.0.0.1", 0, 512);
        auto server = children.spawn(serve_data_until_cancelled(listener, state));
        if (!server)
          co_await weave::fail(server.error());
        auto held = co_await weave::tcp::connect("127.0.0.1", listener.local_port());
        std::array marker{std::byte{254}};
        co_await held.write_all(marker);
        while (state.active == 0)
          co_await weave::sleep_for(1ms);

        std::vector<weave::JoinHandle<void>> clients;
        for (int i = 0; i < 64; ++i) {
          auto client = children.spawn(serve_roundtrip(listener.local_port(), i, state));
          if (!client)
            co_await weave::fail(client.error());
          clients.push_back(std::move(*client));
        }
        for (auto &client : clients)
          co_await std::move(client);
        while (state.failed != 4)
          co_await weave::sleep_for(1ms);
        CHECK(state.completed == 64);
        CHECK(state.active == 1);
        server->cancel();
        co_await std::move(*server);
        CHECK(state.entered >= 185);
        CHECK(state.destroyed == state.entered);
        CHECK(state.cancelled == 1);
      };
      REQUIRE(runtime->run(weave::timeout(5s, weave::scope(body))));
      CHECK_FALSE(runtime->stop_requested());
      REQUIRE(runtime->run(weave::sleep_for(1ms)));
    }
  }
}
