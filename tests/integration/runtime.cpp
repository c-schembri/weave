#include <weave/tcp.hpp>
#include <doctest/doctest.h>
#include <weave/runtime.hpp>
#include "runtime_fixture.hpp"
#include "echo_peer.hpp"
#include <barrier>
#include <set>
#include <condition_variable>
#include <chrono>

static weave::Scheduler test_scheduler()
{
  auto mode = weave::Scheduler::worker_affine;
  SUBCASE("worker-affine")
  {
    mode = weave::Scheduler::worker_affine;
  }
  SUBCASE("work-stealing")
  {
    mode = weave::Scheduler::work_stealing;
  }
  return mode;
}

TEST_CASE_TEMPLATE("Runtime executes on multiple workers and preserves task affinity", Layout, WEAVE_TEST_IO_LAYOUTS)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = test_scheduler()});
  REQUIRE(runtime);
  CHECK(runtime->worker_count() == 4);
  std::barrier gate(4);
  std::vector<weave::JoinHandle<std::thread::id>> jobs;
  for (std::size_t i = 0; i < 4; ++i) {
    auto job = runtime->spawn_on(i, [&gate](weave::Context &ctx) -> weave::Task<std::thread::id> {
      const auto owner = std::this_thread::get_id();
      // A deliberate test-only barrier proves four workers execute concurrently.
      gate.arrive_and_wait();
      for (int j = 0; j < 20; ++j) {
        co_await ctx.yield();
        if (std::this_thread::get_id() != owner)
          co_return std::thread::id{};
      }
      co_return owner;
    });
    REQUIRE(job);
    jobs.push_back(std::move(*job));
  }
  std::set<std::thread::id> owners;
  for (auto &job : jobs) {
    auto result = std::move(job).get();
    REQUIRE(result);
    owners.insert(*result);
  }
  CHECK(owners.size() == 4);
  CHECK_FALSE(owners.contains(std::this_thread::get_id()));
  CHECK_FALSE(owners.contains(std::thread::id{}));
  runtime->join();
}

TEST_CASE_TEMPLATE(
  "Nested spawn and async join never block a worker or migrate the parent",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  const auto mode = test_scheduler();
  for (std::size_t workers : {1, 4}) {
    auto runtime = support::create_runtime<Layout>({.workers = workers, .scheduler = mode});
    REQUIRE(runtime);
    auto parent = runtime->spawn_on(0, [&runtime](weave::Context &ctx) -> weave::Task<int> {
      const auto owner = std::this_thread::get_id();
      int sum = 0;
      for (int i = 0; i < 1000; ++i) {
        auto child = runtime->spawn([i](weave::Context &child_ctx) -> weave::Task<int> {
          if (i % 2)
            co_await child_ctx.yield();
          co_return i;
        });
        if (!child)
          co_return -1;
        if (i % 3)
          co_await ctx.yield();
        sum += co_await std::move(*child);
        if (std::this_thread::get_id() != owner)
          co_return -2;
      }
      co_return sum;
    });
    REQUIRE(parent);
    CHECK(std::move(*parent).get() == 999 * 1000 / 2);
  }
}

static weave::Task<bool> join_from_context(weave::JoinHandle<std::unique_ptr<int>> job)
{
  const auto owner = std::this_thread::get_id();
  auto value = co_await std::move(job);
  co_return value && *value == 42 && owner == std::this_thread::get_id();
}

TEST_CASE_TEMPLATE(
  "Join handles support move-only results and awaiting from a standalone context",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  auto runtime = support::create_runtime<Layout>({.workers = 2, .scheduler = test_scheduler()});
  REQUIRE(runtime);
  auto context = weave::Context::create();
  REQUIRE(context);

  for (int i = 0; i < 100; ++i) {
    auto job = runtime->spawn([](weave::Context &ctx) -> weave::Task<std::unique_ptr<int>> {
      co_await ctx.yield();
      co_return std::make_unique<int>(42);
    });
    REQUIRE(job);
    CHECK(context->run(join_from_context(std::move(*job))) == true);
  }
}

TEST_CASE_TEMPLATE("Concurrent submitters publish results exactly once", Layout, WEAVE_TEST_IO_LAYOUTS)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = test_scheduler()});
  REQUIRE(runtime);
  std::atomic<int> calls = 0, errors = 0;
  std::vector<std::thread> producers;
  for (int t = 0; t < 8; ++t) {
    producers.emplace_back([&] {
      for (int i = 0; i < 200; ++i) {
        auto job = runtime->spawn([&calls, i](weave::Context &ctx) -> weave::Task<int> {
          ++calls;
          co_await ctx.yield();
          co_return i;
        });
        if (!job || std::move(*job).get() != i)
          ++errors;
      }
    });
  }
  for (auto &producer : producers)
    producer.join();
  runtime->join();
  CHECK(calls == 1600);
  CHECK(errors == 0);
}

TEST_CASE_TEMPLATE(
  "Discarded handles remain runtime-owned and join drains accepted tasks",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = test_scheduler()});
  REQUIRE(runtime);
  std::atomic<int> completed = 0;
  for (int i = 0; i < 1000; ++i) {
    auto job = runtime->spawn([&completed](weave::Context &ctx) -> weave::Task<void> {
      co_await ctx.yield();
      ++completed;
    });
    REQUIRE(job);
  }
  runtime->join();
  CHECK(completed == 1000);
  auto rejected = runtime->spawn([](weave::Context &) -> weave::Task<void> {
    co_return;
  });
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == std::errc::operation_canceled);
  runtime->join();
}

TEST_CASE_TEMPLATE(
  "Shutdown cancels pending accept and preserves the result after runtime destruction",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  std::optional<weave::JoinHandle<void>> saved;
  std::atomic<bool> waiting = false;
  {
    auto runtime = support::create_runtime<Layout>({.workers = 2, .scheduler = test_scheduler()});
    REQUIRE(runtime);
    auto job = runtime->spawn([&waiting](weave::Context &ctx) -> weave::Task<void> {
      auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
      if (!listener)
        co_await weave::fail(listener.error());
      waiting.store(true, std::memory_order_release);
      waiting.notify_one();
      (void)co_await listener->accept();
    });
    REQUIRE(job);
    saved.emplace(std::move(*job));
    waiting.wait(false, std::memory_order_acquire);
    runtime->shutdown();
    CHECK(saved->ready());
  }
  auto result = std::move(*saved).get();
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
}

TEST_CASE_TEMPLATE(
  "Shutdown drains cancelled reads before reclaiming worker sockets and frames",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  const auto mode = test_scheduler();
  for (bool skip : {false, true}) {
    support::EchoPeer peer;
    auto runtime = support::create_runtime<Layout>(
      {.workers = 2, .scheduler = mode, .context = {.skip_successful_completions = skip}});
    REQUIRE(runtime);
    std::atomic<bool> waiting = false;
    auto job = runtime->spawn([&](weave::Context &ctx) -> weave::Task<void> {
      auto stream = co_await weave::tcp::connect(ctx, "127.0.0.1", peer.port());
      std::array<std::byte, 16> buffer{};
      waiting.store(true, std::memory_order_release);
      waiting.notify_one();
      (void)co_await stream.read(buffer);
    });
    REQUIRE(job);
    waiting.wait(false, std::memory_order_acquire);
    runtime->shutdown();
    auto result = std::move(*job).get();
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
    peer.join();
    CHECK(peer.ok());
  }
}

TEST_CASE_TEMPLATE("A worker may request cooperative stop and later I/O is rejected", Layout, WEAVE_TEST_IO_LAYOUTS)
{
  auto runtime = support::create_runtime<Layout>({.workers = 2, .scheduler = test_scheduler()});
  REQUIRE(runtime);
  auto invalid = runtime->spawn_on(2, [](weave::Context &) -> weave::Task<void> {
    co_return;
  });
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == std::errc::invalid_argument);
  auto job = runtime->spawn([&runtime](weave::Context &ctx) -> weave::Task<bool> {
    runtime->request_stop();
    co_await ctx.yield();
    // A wake and this yield can be in the same batch; let the driver observe stop.
    while (!ctx.stop_requested())
      co_await ctx.yield();
    auto connection = co_await weave::as_result(weave::tcp::connect(ctx, "127.0.0.1", 1));
    co_return !connection && connection.error() == std::errc::operation_canceled;
  });
  REQUIRE(job);
  CHECK(std::move(*job).get() == true);
  runtime->join();
  CHECK(runtime->stop_requested());
}

static weave::Task<bool> implicit_echo_peer(weave::Task<weave::TcpStream> setup)
{
  auto stream = co_await std::move(setup);
  std::vector<std::byte> tx(32768, std::byte{0x35}), rx(tx.size());
  for (int repeat = 0; repeat < 20; ++repeat) {
    co_await stream.write_all(tx);
    co_await stream.read_exactly(rx);
    if (tx != rx)
      co_return false;
  }
  co_return true;
}

static weave::Task<bool> implicit_runtime_roundtrip()
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0, 512);
  bool matched = false;
  auto server = [&]() -> weave::Task<void> {
    auto stream = co_await listener.accept();
    std::array<std::byte, 4> data{};
    co_await stream.read_exactly(data);
    co_await stream.write_all(data);
  };
  auto client = [&]() -> weave::Task<void> {
    auto stream = co_await weave::tcp::connect("127.0.0.1", listener.local_port());
    std::array<std::byte, 4> tx{std::byte{0x31}}, rx{};
    co_await stream.write_all(tx);
    co_await stream.read_exactly(rx);
    matched = tx == rx;
  };
  co_await weave::when_all(server(), client());
  co_return matched;
}

TEST_CASE_TEMPLATE(
  "Runtime run resolves implicit TCP setup without a Context-taking factory",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  const auto mode = test_scheduler();
  for (std::size_t workers : {1, 4}) {
    for (bool skip : {false, true}) {
      auto runtime = support::create_runtime<Layout>(
        {.workers = workers, .scheduler = mode, .context = {.skip_successful_completions = skip}});
      REQUIRE(runtime);
      REQUIRE(weave::detail::current_context == nullptr);
      CHECK(runtime->run(implicit_runtime_roundtrip()) == true);
      CHECK(weave::detail::current_context == nullptr);
    }
  }
}

TEST_CASE_TEMPLATE(
  "Implicit TCP connections constructed on the caller execute on all runtime workers",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  const auto mode = test_scheduler();
  for (bool skip : {false, true}) {
    auto runtime = support::create_runtime<Layout>(
      {.workers = 4, .scheduler = mode, .context = {.skip_successful_completions = skip}});
    REQUIRE(runtime);
    std::vector<std::unique_ptr<support::EchoPeer>> peers;
    std::vector<weave::JoinHandle<bool>> jobs;
    for (std::size_t i = 0; i < 8; ++i) {
      peers.push_back(std::make_unique<support::EchoPeer>(997));
      auto job = runtime->spawn_on(i % 4, implicit_echo_peer(weave::tcp::connect("127.0.0.1", peers.back()->port())));
      REQUIRE(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      CHECK(std::move(job).get() == true);
    runtime->join();
    for (auto &peer : peers) {
      peer->join();
      CHECK(peer->ok());
    }
  }
}

TEST_CASE_TEMPLATE(
  "A captured socket is destroyed on its worker before an external join observes completion",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  support::EchoPeer peer;
  auto runtime = support::create_runtime<Layout>({.workers = 2, .scheduler = test_scheduler()});
  REQUIRE(runtime);
  auto parent = runtime->spawn_on(0, [&](weave::Context &ctx) -> weave::Task<weave::JoinHandle<bool>> {
    auto stream = co_await weave::as_result(weave::tcp::connect(ctx, "127.0.0.1", peer.port()));
    if (!stream)
      co_return std::unexpected(stream.error());
    auto job = runtime->spawn_on(0, [socket = std::move(*stream)](weave::Context &) mutable -> weave::Task<bool> {
      std::array<std::byte, 1> tx{std::byte{0x31}}, rx{};
      if (!(co_await weave::as_result(socket.write_all(tx))))
        co_return false;
      co_return (co_await weave::as_result(socket.read_exactly(rx))) && tx == rx;
    });
    if (!job)
      co_await weave::fail(job.error());
    co_return std::move(*job);
  });
  REQUIRE(parent);
  auto child = std::move(*parent).get();
  REQUIRE(child);
  CHECK(std::move(*child).get() == true);
  peer.join();
  CHECK(peer.ok());
  runtime->join();
}

TEST_CASE_TEMPLATE(
  "Stop racing external submissions rejects new tasks and drains every accepted task",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = test_scheduler()});
  REQUIRE(runtime);
  std::atomic<int> accepted = 0, completed = 0, cancelled = 0, errors = 0;
  std::atomic<bool> started = false;
  std::vector<std::thread> producers;
  for (int i = 0; i < 8; ++i) {
    producers.emplace_back([&] {
      for (int j = 0; j < 500; ++j) {
        auto job = runtime->spawn([&completed](weave::Context &ctx) -> weave::Task<void> {
          co_await ctx.yield();
          ++completed;
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
  runtime->request_stop();
  runtime->join();
  for (auto &producer : producers)
    producer.join();
  CHECK(accepted > 0);
  CHECK(accepted == completed + cancelled);
  CHECK(errors == 0);
}

TEST_CASE_TEMPLATE("Scheduler selection is explicit and invalid modes are rejected", Layout, WEAVE_TEST_IO_LAYOUTS)
{
  auto normal = support::create_runtime<Layout>({.workers = 1});
  REQUIRE(normal);
  CHECK(normal->scheduler() == weave::Scheduler::worker_affine);
  auto invalid = support::create_runtime<Layout>({.workers = 1, .scheduler = static_cast<weave::Scheduler>(99)});
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == std::errc::invalid_argument);
}

TEST_CASE_TEMPLATE("Idle workers steal a burst from a pinned parent's local queue", Layout, WEAVE_TEST_IO_LAYOUTS)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
  REQUIRE(runtime);
  CHECK(runtime->scheduler() == weave::Scheduler::work_stealing);
  std::barrier gate(4);
  auto parent = runtime->spawn_on(0, [&](weave::Context &) -> weave::Task<bool> {
    const auto owner = std::this_thread::get_id();
    std::vector<weave::JoinHandle<std::thread::id>> children;
    for (int i = 0; i < 3; ++i) {
      auto child = runtime->spawn([&](weave::Context &) -> weave::Task<std::thread::id> {
        const auto thread = std::this_thread::get_id();
        gate.arrive_and_wait();
        co_return thread;
      });
      if (!child)
        co_return false;
      children.push_back(std::move(*child));
    }
    // Test-only blocking: all children were queued locally, so thieves must help.
    gate.arrive_and_wait();
    std::set<std::thread::id> threads{owner};
    for (auto &child : children)
      threads.insert(co_await std::move(child));
    co_return threads.size() == 4 && std::this_thread::get_id() == owner;
  });
  REQUIRE(parent);
  CHECK(std::move(*parent).get() == true);
}

TEST_CASE_TEMPLATE(
  "A yielded task can migrate with a live socket and retain its original Context reference",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  support::EchoPeer peer;
  auto runtime = support::create_runtime<Layout>({.workers = 2, .scheduler = weave::Scheduler::work_stealing});
  REQUIRE(runtime);
  std::array<std::thread::id, 2> owners;
  for (std::size_t i = 0; i < owners.size(); ++i) {
    auto job = runtime->spawn_on(i, [](weave::Context &) -> weave::Task<std::thread::id> {
      co_return std::this_thread::get_id();
    });
    REQUIRE(job);
    auto result = std::move(*job).get();
    REQUIRE(result);
    owners[i] = *result;
  }
  auto job = runtime->spawn([&](weave::Context &ctx) -> weave::Task<bool> {
    auto stream = co_await weave::as_result(weave::tcp::connect("127.0.0.1", peer.port()));
    if (!stream)
      co_return false;
    const auto original = std::this_thread::get_id();
    const std::size_t index = original == owners[0] ? 0 : 1;
    std::atomic<bool> blocked = false, release = false;
    auto blocker = runtime->spawn_on(index, [&](weave::Context &) -> weave::Task<void> {
      blocked.store(true, std::memory_order_release);
      release.wait(false, std::memory_order_acquire);
      co_return;
    });
    if (!blocker)
      co_return false;
    while (!blocked.load(std::memory_order_acquire))
      co_await ctx.yield();
    const bool migrated = original != std::this_thread::get_id();
    const bool configured = static_cast<bool>(stream->no_delay());
    release.store(true, std::memory_order_release);
    release.notify_one();
    co_await std::move(*blocker);
    std::array<std::byte, 1024> tx{}, rx{};
    tx.fill(std::byte{0x71});
    if (!(co_await weave::as_result(stream->write_all(tx))))
      co_return false;
    if (!(co_await weave::as_result(stream->read_exactly(rx))))
      co_return false;
    co_return migrated && configured && tx == rx;
  });
  REQUIRE(job);
  CHECK(std::move(*job).get() == true);
  runtime->join();
  peer.join();
  CHECK(peer.ok());
}

TEST_CASE_TEMPLATE(
  "Stealing serializes when_all children while servicing duplex I/O and joins",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  for (bool skip : {false, true}) {
    auto runtime = support::create_runtime<Layout>(
      {.workers = 4, .scheduler = weave::Scheduler::work_stealing, .context = {.skip_successful_completions = skip}});
    REQUIRE(runtime);
    std::vector<std::unique_ptr<support::EchoPeer>> peers;
    std::vector<weave::JoinHandle<bool>> jobs;
    for (int i = 0; i < 8; ++i) {
      peers.push_back(std::make_unique<support::EchoPeer>(997));
      auto job = runtime->spawn([port = peers.back()->port()](weave::Context &ctx) -> weave::Task<bool> {
        auto stream = co_await weave::as_result(weave::tcp::connect(ctx, "127.0.0.1", port));
        if (!stream)
          co_return false;
        std::vector<std::byte> tx(256 * 1024, std::byte{0x49}), rx(tx.size());
        for (int repeat = 0; repeat < 5; ++repeat) {
          bool sent = false, read = false;
          auto writer = [&]() -> weave::Task<void> {
            co_await ctx.yield();
            sent = static_cast<bool>(co_await weave::as_result(stream->write_all(tx)));
          };
          auto reader = [&]() -> weave::Task<void> {
            co_await ctx.yield();
            read = static_cast<bool>(co_await weave::as_result(stream->read_exactly(rx)));
          };
          co_await weave::when_all(writer(), reader());
          if (!sent || !read || tx != rx)
            co_return false;
        }
        co_return true;
      });
      REQUIRE(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      CHECK(std::move(job).get() == true);
    runtime->join();
    for (auto &peer : peers) {
      peer->join();
      CHECK(peer->ok());
    }
  }
}

TEST_CASE_TEMPLATE(
  "A stealing listener transfers accepted sockets to handlers pinned to another worker",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  for (bool skip : {false, true}) {
    auto runtime = support::create_runtime<Layout>(
      {.workers = 4, .scheduler = weave::Scheduler::work_stealing, .context = {.skip_successful_completions = skip}});
    REQUIRE(runtime);
    std::atomic<int> port = -1;
    auto server = runtime->spawn_on(0, [&](weave::Context &ctx) -> weave::Task<bool> {
      auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
      port.store(listener ? listener->local_port() : 0, std::memory_order_release);
      port.notify_one();
      if (!listener)
        co_return false;
      std::vector<weave::JoinHandle<bool>> handlers;
      for (int i = 0; i < 8; ++i) {
        auto accepted = co_await weave::as_result(listener->accept());
        if (!accepted)
          co_return false;
        auto handler = runtime->spawn_on(
          1,
          [socket = std::move(*accepted)](weave::Context &worker) mutable -> weave::Task<bool> {
            if (!socket.no_delay())
              co_return false;
            co_await worker.yield();
            std::array<std::byte, 1024> data{};
            if (!(co_await weave::as_result(socket.read_exactly(data))))
              co_return false;
            co_return static_cast<bool>(co_await weave::as_result(socket.write_all(data)));
          });
        if (!handler)
          co_return false;
        handlers.push_back(std::move(*handler));
      }
      bool ok = true;
      for (auto &handler : handlers)
        ok = (co_await std::move(handler)) && ok;
      co_return ok;
    });
    REQUIRE(server);
    port.wait(-1, std::memory_order_acquire);
    REQUIRE(port > 0);
    std::vector<weave::JoinHandle<bool>> clients;
    for (int i = 0; i < 8; ++i) {
      auto client = runtime->spawn(
        [target = static_cast<weave::u16>(port.load())](weave::Context &ctx) -> weave::Task<bool> {
          auto stream = co_await weave::as_result(weave::tcp::connect(ctx, "127.0.0.1", target));
          if (!stream)
            co_return false;
          std::array<std::byte, 1024> tx{}, rx{};
          tx.fill(std::byte{0x18});
          if (!(co_await weave::as_result(stream->write_all(tx))))
            co_return false;
          co_return (co_await weave::as_result(stream->read_exactly(rx))) && tx == rx;
        });
      REQUIRE(client);
      clients.push_back(std::move(*client));
    }
    for (auto &client : clients)
      CHECK(std::move(client).get() == true);
    CHECK(std::move(*server).get() == true);
    runtime->join();
  }
}

TEST_CASE_TEMPLATE(
  "Both schedulers preserve immediate errors, overlap guards, cancellation and EOF",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  const auto mode = test_scheduler();
  for (bool skip : {false, true}) {
    support::EchoPeer peer;
    auto runtime = support::create_runtime<Layout>(
      {.workers = 4, .scheduler = mode, .context = {.skip_successful_completions = skip}});
    REQUIRE(runtime);
    auto job = runtime->spawn([&](weave::Context &ctx) -> weave::Task<bool> {
      auto invalid = co_await weave::as_result(weave::tcp::connect(ctx, "bad host", 0));
      if (invalid || invalid.error() != std::errc::invalid_argument)
        co_return false;
      auto stream = co_await weave::as_result(weave::tcp::connect(ctx, "127.0.0.1", peer.port()));
      if (!stream)
        co_return false;
      bool cancelled = false, guarded = false;
      auto reader = [&]() -> weave::Task<void> {
        std::array<std::byte, 1> data{};
        auto read = co_await weave::as_result(stream->read(data));
        cancelled = !read && read.error().value() == support::native_cancelled;
      };
      auto cancel = [&]() -> weave::Task<void> {
        std::array<std::byte, 1> data{};
        auto read = co_await weave::as_result(stream->read(data));
        guarded = !read && read.error() == std::errc::operation_in_progress && !stream->close() && stream->cancel();
      };
      co_await weave::when_all(reader(), cancel());
      if (!cancelled || !guarded || !stream->shutdown_send())
        co_return false;
      std::array<std::byte, 1> data{};
      auto eof = co_await weave::as_result(stream->read(data));
      if (!eof || *eof != 0 || !stream->close())
        co_return false;
      auto closed = co_await weave::as_result(stream->read(data));
      co_return !closed && closed.error().value() == support::native_not_socket;
    });
    REQUIRE(job);
    CHECK(std::move(*job).get() == true);
    runtime->join();
    peer.join();
    CHECK(peer.ok());
  }
}

TEST_CASE_TEMPLATE(
  "Runtime join includes context-owned tasks and closes both submission paths",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  auto runtime = support::create_runtime<Layout>({.workers = 4, .scheduler = test_scheduler()});
  REQUIRE(runtime);
  std::atomic<bool> release = false, joined = false;
  std::atomic<int> finished = 0;
  std::vector<weave::JoinHandle<int>> jobs;
  for (std::size_t i = 0; i < 2; ++i) {
    auto parent = runtime->spawn_on(i, [&, i](weave::Context &ctx) -> weave::Task<weave::JoinHandle<int>> {
      auto job = ctx.spawn([&, i](weave::Context &local) -> weave::Task<int> {
        while (!release.load(std::memory_order_acquire))
          co_await local.yield();
        auto rejected = local.spawn([](weave::Context &) -> weave::Task<void> {
          co_return;
        });
        CHECK_FALSE(rejected);
        CHECK(rejected.error() == std::errc::operation_canceled);
        ++finished;
        co_return static_cast<int>(i);
      });
      if (!job)
        co_await weave::fail(job.error());
      co_return std::move(*job);
    });
    REQUIRE(parent);
    auto child = std::move(*parent).get();
    REQUIRE(child);
    jobs.push_back(std::move(*child));
  }
  std::thread joining([&] {
    runtime->join();
    joined.store(true, std::memory_order_release);
  });
  for (;;) {
    auto probe = runtime->spawn([](weave::Context &) -> weave::Task<void> {
      co_return;
    });
    if (!probe) {
      CHECK(probe.error() == std::errc::operation_canceled);
      break;
    }
    std::this_thread::yield();
  }
  CHECK_FALSE(joined.load(std::memory_order_acquire));
  release.store(true, std::memory_order_release);
  joining.join();
  CHECK(finished == 2);
  for (int i = 0; i < 2; ++i) {
    REQUIRE(jobs[i].ready());
    CHECK(std::move(jobs[i]).get() == i);
  }
}

TEST_CASE_TEMPLATE(
  "Runtime shutdown drains context-owned pending sockets under both schedulers",
  Layout,
  WEAVE_TEST_IO_LAYOUTS)
{
  const auto mode = test_scheduler();
  for (bool skip : {false, true}) {
    auto runtime = support::create_runtime<Layout>(
      {.workers = 4, .scheduler = mode, .context = {.skip_successful_completions = skip}});
    REQUIRE(runtime);
    std::atomic<bool> waiting = false;
    std::atomic<int> destroyed = 0;
    auto parent = runtime->spawn([&](weave::Context &ctx) -> weave::Task<weave::JoinHandle<void>> {
      auto job = ctx.spawn([&](weave::Context &local) -> weave::Task<void> {
        struct Guard {
          std::atomic<int> &destroyed;
          ~Guard()
          {
            ++destroyed;
          }
        } guard{destroyed};
        auto listener = weave::tcp::listen(local, "127.0.0.1", 0);
        if (!listener)
          co_await weave::fail(listener.error());
        waiting.store(true, std::memory_order_release);
        waiting.notify_one();
        (void)co_await listener->accept();
        FAIL("Context-owned accept resumed after runtime shutdown");
      });
      if (!job)
        co_await weave::fail(job.error());
      co_return std::move(*job);
    });
    REQUIRE(parent);
    auto child = std::move(*parent).get();
    REQUIRE(child);
    waiting.wait(false, std::memory_order_acquire);
    runtime->shutdown();
    auto result = std::move(*child).get();
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
    CHECK(destroyed == 1);
  }
}

static weave::Task<void> detached_pending_accept(
  std::atomic<int> &destroyed,
  std::atomic<bool> &started,
  std::atomic<weave::Context *> &binding)
{
  struct Guard {
    std::atomic<int> &destroyed;

    ~Guard()
    {
      ++destroyed;
    }
  } guard{destroyed};

  binding.store(weave::detail::current_context, std::memory_order_release);
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  started.store(true, std::memory_order_release);
  (void)co_await listener.accept();
  FAIL("Detached accept resumed after cancellation");
}

TEST_CASE_TEMPLATE("Runtime shutdown drains explicit and scope-detached native work", Layout, WEAVE_TEST_IO_LAYOUTS)
{
  const auto mode = test_scheduler();
  const std::array completion_modes{false, true};
  const std::array targets{0, 1, 2};
  const std::array submission_modes{false, true};

  for (bool skip : completion_modes) {
    for (int target : targets) {
      for (bool direct : submission_modes) {
        std::atomic<int> destroyed = 0, observed = 0;
        std::atomic<bool> started = false;
        std::atomic<weave::Context *> binding = nullptr;
        auto runtime = support::create_runtime<Layout>(
          {.workers = 2, .scheduler = mode, .context = {.skip_successful_completions = skip}});
        REQUIRE(runtime);
        auto launch = [&](weave::Context &) {
          return detached_pending_accept(destroyed, started, binding);
        };
        auto on_error = [&](weave::Error error) noexcept {
          CHECK(error == std::errc::operation_canceled);
          ++observed;
        };
        auto submit = runtime->spawn_on(0, [&, launch, on_error](weave::Context &ctx) -> weave::Task<void> {
          if (target == 2) {
            if (direct)
              weave::detach(launch(ctx), on_error);
            else
              weave::detach(launch, on_error);
          } else if (target == 1) {
            if (direct)
              ctx.detach(launch(ctx), on_error);
            else
              ctx.detach(launch, on_error);
          } else {
            if (direct)
              runtime->detach_on(0, launch(ctx), on_error);
            else
              runtime->detach_on(0, launch, on_error);
          }
          co_return;
        });
        REQUIRE(submit);
        REQUIRE(std::move(*submit).get());
        auto pending = runtime->spawn_on(0, [&](weave::Context &ctx) -> weave::Task<bool> {
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};

          // The detached root may bind its listener on a stealing worker, not worker zero.
          while (std::chrono::steady_clock::now() < deadline) {
            if (started.load(std::memory_order_acquire)) {
              const auto counters = binding.load(std::memory_order_acquire)->metrics();
              if (counters.submitted > counters.completed)
                co_return true;
            }

            co_await ctx.yield();
          }

          co_return false;
        });
        REQUIRE(pending);
        REQUIRE(std::move(*pending).get() == true);
        CHECK(observed == 0);
        runtime->shutdown();
        CHECK(observed == 1);
        CHECK(destroyed == 1);
      }
    }
  }
}

#if defined(_WIN32)
TEST_CASE("Shared IOCP services a pending socket while its original worker is blocked")
{
  for (bool skip : {false, true}) {
    auto runtime = weave::Runtime::create(
      {.workers = 2,
        .scheduler = weave::Scheduler::work_stealing,
        .context = {.skip_successful_completions = skip},
        .io_layout = weave::IoLayout::shared});
    REQUIRE(runtime);
    std::atomic<int> port = -1;
    std::atomic<bool> reading = false, blocked = false;
    std::mutex mutex;
    std::condition_variable wake;
    bool completed = false;

    auto server = runtime->spawn_on(0, [&](weave::Context &) -> weave::Task<void> {
      auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
      port.store(listener.local_port(), std::memory_order_release);
      port.notify_one();
      auto socket = co_await listener.accept();
      auto reader = runtime->spawn([&, socket = std::move(socket)]() mutable -> weave::Task<void> {
        std::array<std::byte, 1> byte{};
        reading.store(true, std::memory_order_release);
        reading.notify_one();
        co_await socket.read_exactly(byte);
        CHECK(byte[0] == std::byte{0x71});
        {
          std::lock_guard lock(mutex);
          completed = true;
        }
        wake.notify_one();
      });
      if (!reader)
        co_await weave::fail(reader.error());
      co_await std::move(*reader);
    });
    REQUIRE(server);
    port.wait(-1, std::memory_order_acquire);
    auto client = support::connect(static_cast<weave::u16>(port.load()));
    reading.wait(false, std::memory_order_acquire);
    auto blocker = runtime->spawn_on(0, [&](weave::Context &) -> weave::Task<bool> {
      // Test-only blocking exposes whether another worker can service worker zero's native port.
      std::unique_lock lock(mutex);
      blocked.store(true, std::memory_order_release);
      blocked.notify_one();
      co_return wake.wait_for(lock, std::chrono::seconds(3), [&] {
        return completed;
      });
    });
    REQUIRE(blocker);
    blocked.wait(false, std::memory_order_acquire);
    CHECK(support::write_all(client, "q", 1));
    CHECK(std::move(*blocker).get() == true);
    CHECK(std::move(*server).get());
    support::close_socket(client);
    runtime->join();
  }
}
#endif
