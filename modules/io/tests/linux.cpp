#include "linux/uring.hpp"
#include "linux/resolve.hpp"
#include "linux/socket.hpp"
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <fcntl.h>
#include <sys/resource.h>

TEST_CASE("Linux Context reports native setup failure without publishing execution state")
{
  rlimit limits{};
  REQUIRE(getrlimit(RLIMIT_NOFILE, &limits) == 0);
  auto restricted = limits;
  restricted.rlim_cur = 0;
  REQUIRE(setrlimit(RLIMIT_NOFILE, &restricted) == 0);
  auto failed = weave::Context::create();
  const auto restored = setrlimit(RLIMIT_NOFILE, &limits);

  REQUIRE(restored == 0);
  REQUIRE_FALSE(failed);
  CHECK(failed.error() == std::errc::too_many_files_open);
  CHECK(weave::detail::current_context == nullptr);
  CHECK(weave::Context::create());
}

TEST_CASE("Linux Context drains its wake read and releases both native descriptors")
{
  constexpr std::array completion_modes{false, true};
  for (bool skip : completion_modes) {
    int ring;
    int wake;
    {
      auto ctx = weave::Context::create({.skip_successful_completions = skip});
      REQUIRE(ctx);
      auto &state = weave::detail::IoAccess::state(*ctx);
      CHECK(state.options_.skip_successful_completions == skip);
      ring = state.ring_.ring_fd;
      wake = state.wake_fd_;
      CHECK(fcntl(ring, F_GETFD) >= 0);
      CHECK(fcntl(wake, F_GETFD) >= 0);
      CHECK(ctx->run(weave::when_all()));
    }
    CHECK(fcntl(ring, F_GETFD) == -1);
    CHECK(errno == EBADF);
    CHECK(fcntl(wake, F_GETFD) == -1);
    CHECK(errno == EBADF);
  }
}

struct NativeNoop {
  weave::detail::Operation operation;

  bool await_ready() const noexcept
  {
    return false;
  }

  void await_suspend(std::coroutine_handle<> continuation) noexcept
  {
    operation.context = weave::detail::current_context;
    operation.event.state = continuation.address();
    operation.event.invoke = [](void *state) noexcept {
      std::coroutine_handle<>::from_address(state).resume();
    };
    operation.prepare = [](io_uring_sqe *entry, weave::detail::Operation &) noexcept {
      io_uring_prep_nop(entry);
    };
    weave::detail::IoAccess::submit(operation);
  }

  void await_resume() noexcept
  {
    weave::detail::require(operation.result == 0);
  }
};

static weave::Task<void> native_noop(unsigned &completed)
{
  NativeNoop operation;
  co_await operation;
  ++completed;
}

TEST_CASE("Linux completion backpressure drains thousands of frames through a tiny ring")
{
  auto ctx = weave::detail::IoAccess::create_context({}, 2);
  REQUIRE(ctx);
  unsigned completed = 0;
  auto burst = [&]() -> weave::Task<void> {
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned i = 0; i < 4096; ++i) {
      auto job = ctx->spawn(native_noop(completed));
      if (!job)
        co_await weave::fail(job.error());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      co_await std::move(job);
  };
  REQUIRE(ctx->run(burst()));
  CHECK(completed == 4096);
  CHECK(ctx->metrics().submitted == 4096);
  CHECK(ctx->metrics().completed == 4096);
  CHECK(weave::detail::IoAccess::state(*ctx).operations_.empty());
}

static weave::Task<std::size_t> native_receive(
  weave::Context &ctx,
  int socket,
  std::span<std::byte> buffer,
  bool &receive_nonempty)
{
  weave::detail::SocketIoAwaiter operation{weave::detail::SocketIoAwaiter::receive, ctx, socket};
  operation.buffer = buffer.data();
  operation.size = buffer.size();
  operation.receive_nonempty = &receive_nonempty;
  auto result = co_await operation;
  if (!result)
    co_await weave::fail(result.error());
  co_return *result;
}

TEST_CASE("Linux timed submission without a CQE still expires and drains native cancellation")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  if (!ctx)
    return;
  std::array<int, 2> sockets{};
  const auto created = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data());
  REQUIRE(created == 0);
  if (created != 0)
    return;

  weave::CancelSource stop;
  weave::detail::TimerRecord timer;
  timer.context = &*ctx;
  timer.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{10};
  timer.event.state = &stop;
  timer.event.invoke = [](void *state) noexcept {
    static_cast<weave::CancelSource *>(state)->cancel();
  };
  // Install the deadline before running, without a wake CQE that could hide the empty timed return.
  weave::detail::IoAccess::timers(*ctx).insert(timer);
  std::array<std::byte, 1> buffer;
  bool receive_nonempty = false;
  auto operation = native_receive(*ctx, sockets[0], buffer, receive_nonempty);
  weave::detail::TaskAccess::bind(operation, stop.token());
  auto result = ctx->run(std::move(operation));
  CHECK(!result);
  if (!result)
    CHECK(result.error() == std::errc::operation_canceled);
  CHECK(receive_nonempty);
  CHECK(timer.index == std::numeric_limits<std::size_t>::max());
  CHECK(ctx->metrics().submitted == 1);
  CHECK(ctx->metrics().completed == 1);
  CHECK(weave::detail::IoAccess::state(*ctx).operations_.empty());
  CHECK(ctx->run(weave::when_all()));
  for (int socket : sockets)
    CHECK(::close(socket) == 0);
}

struct ContextStopReceive : weave::detail::SocketIoAwaiter {
  bool before_admission;
  bool &destroyed;

  ContextStopReceive(weave::Context &ctx, int socket, bool before, bool &destroyed)
      : SocketIoAwaiter(receive, ctx, socket), before_admission(before), destroyed(destroyed)
  {
  }

  ~ContextStopReceive()
  {
    CHECK(operation.native_done);
    CHECK_FALSE(operation.cancel_pending);
    destroyed = true;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> continuation) noexcept
  {
    // This standalone owner cannot dispatch a CQE until await_suspend returns.
    if (!before_admission) {
      const bool pending = SocketIoAwaiter::await_suspend(continuation);
      CHECK(pending);
      context.request_stop();
      CHECK(operation.cancelled);
      return pending;
    }

    operation.event.state = continuation.address();
    operation.event.invoke = [](void *state) noexcept {
      std::coroutine_handle<>::from_address(state).resume();
    };
    operation.event.executor = weave::detail::current_executor;
    context.request_stop();
    // Bypass the socket awaiter's early-stop check to exercise backend admission.
    weave::detail::IoAccess::submit(operation);
    CHECK(operation.cancelled);
    return true;
  }
};

static weave::Task<void> context_stop_receive(weave::Context &ctx, int socket, bool before_admission, bool &destroyed)
{
  std::array<std::byte, 1> buffer{};
  ContextStopReceive operation{ctx, socket, before_admission, destroyed};
  operation.buffer = buffer.data();
  operation.size = buffer.size();
  auto received = co_await operation;
  CHECK_FALSE(destroyed);
  CHECK(operation.operation.native_done);
  CHECK_FALSE(operation.operation.cancel_pending);
  if (!received)
    co_await weave::fail(received.error());
  FAIL("Context-stopped receive returned data");
}

static void check_context_stop_receive(bool before_admission)
{
  constexpr std::array completion_modes{false, true};
  for (bool skip : completion_modes) {
    CAPTURE(skip);
    CAPTURE(before_admission);
    auto ctx = weave::Context::create({.skip_successful_completions = skip});
    REQUIRE(ctx);
    if (!ctx)
      return;

    std::array<int, 2> sockets{};
    const auto created = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data());
    REQUIRE(created == 0);
    if (created != 0)
      return;

    bool destroyed = false;
    auto operation = context_stop_receive(*ctx, sockets[0], before_admission, destroyed);
    weave::detail::TaskAccess::bind(operation, {});
    auto result = ctx->run(std::move(operation));
    CHECK_FALSE(result);
    if (!result)
      CHECK(result.error() == std::errc::operation_canceled);
    CHECK(destroyed);
    CHECK(ctx->stop_requested());
    const weave::u64 submitted = before_admission ? 0 : 1;
    CHECK(ctx->metrics().submitted == submitted);
    CHECK(ctx->metrics().completed == submitted);
    CHECK(weave::detail::IoAccess::state(*ctx).operations_.empty());
    for (int socket : sockets)
      CHECK(::close(socket) == 0);
  }
}

TEST_CASE("Linux Context stop drains native receive with an empty task token")
{
  check_context_stop_receive(false);
}

TEST_CASE("Linux backend rejects native submission after Context stop")
{
  check_context_stop_receive(true);
}

TEST_CASE("Linux receive hints preserve staged bytes, partial reads and EOF")
{
  const std::array hints{false, true};
  for (bool receive_nonempty : hints) {
    CAPTURE(receive_nonempty);
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    if (!ctx)
      return;

    std::array<int, 2> sockets{};
    const auto created = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data());
    REQUIRE(created == 0);
    if (created != 0)
      return;

    const std::array sent{std::byte{17}, std::byte{43}, std::byte{91}};
    const auto written = ::send(sockets[1], sent.data(), sent.size(), MSG_NOSIGNAL);
    REQUIRE(written == static_cast<ssize_t>(sent.size()));
    if (written != static_cast<ssize_t>(sent.size())) {
      for (int socket : sockets)
        CHECK(::close(socket) == 0);
      return;
    }

    auto read = [&]() -> weave::Task<void> {
      std::array<std::byte, 3> received{};
      auto first = co_await native_receive(*ctx, sockets[0], std::span{received}.first(1), receive_nonempty);
      CHECK(first == 1);
      auto remaining = co_await native_receive(*ctx, sockets[0], std::span{received}.subspan(1), receive_nonempty);
      CHECK(remaining == 2);
      CHECK(received == sent);
      CHECK_FALSE(receive_nonempty);

      const auto stopped = ::shutdown(sockets[1], SHUT_WR);
      const auto error = stopped == 0 ? 0 : errno;
      CHECK(stopped == 0);
      if (stopped != 0)
        co_await weave::fail(std::error_code{error, std::generic_category()});
      CHECK((co_await native_receive(*ctx, sockets[0], received, receive_nonempty)) == 0);
    };
    CHECK(ctx->run(weave::timeout(std::chrono::seconds{1}, read())));
    CHECK(ctx->metrics().submitted == 3);
    CHECK(ctx->metrics().completed == 3);
    CHECK(weave::detail::IoAccess::state(*ctx).operations_.empty());
    for (int socket : sockets)
      CHECK(::close(socket) == 0);
  }
}

struct ResolverFixture {
  weave::detail::ResolverRequest *request = nullptr;
  weave::detail::SocketAddress address = weave::detail::socket_address({weave::IpAddress::loopback_v4(), 123});
  addrinfo result{};
  std::atomic<bool> submitted = false;
  std::atomic<bool> cancelled = false;
  unsigned queries = 0;
  unsigned releases = 0;
  int error = 0;
  bool immediate = false;

  ResolverFixture()
  {
    result.ai_addr = address.data();
    result.ai_addrlen = address.size;
  }

  weave::detail::ResolverApi api()
  {
    return {
      this,
      [](void *state, weave::detail::ResolverRequest &request) noexcept {
        auto &fixture = *static_cast<ResolverFixture *>(state);
        ++fixture.queries;
        fixture.request = &request;
        request.native.ar_result = &fixture.result;
        fixture.submitted.store(true, std::memory_order_release);
        fixture.submitted.notify_one();
        if (fixture.immediate)
          request.notify(request);
        return 0;
      },
      [](void *state, weave::detail::ResolverRequest &) noexcept {
        auto &fixture = *static_cast<ResolverFixture *>(state);
        CHECK(fixture.releases == 0);
        fixture.cancelled.store(true, std::memory_order_release);
        fixture.cancelled.notify_one();
        return EAI_NOTCANCELED;
      },
      [](void *state, weave::detail::ResolverRequest &) noexcept {
        return static_cast<ResolverFixture *>(state)->error;
      },
      [](void *state, addrinfo *result) noexcept {
        auto &fixture = *static_cast<ResolverFixture *>(state);
        CHECK(result == &fixture.result);
        ++fixture.releases;
      }};
  }
};

TEST_CASE("Linux resolver handles early and delayed notifications, including lookup failure")
{
  constexpr std::array notification_modes{false, true};
  constexpr std::array errors{0, EAI_NONAME};
  for (bool immediate : notification_modes) {
    for (int error : errors) {
      for (int repetition = 0; repetition < 20; ++repetition) {
        auto ctx = weave::Context::create();
        REQUIRE(ctx);
        ResolverFixture fixture;
        fixture.immediate = immediate;
        fixture.error = error;
        std::thread callback;
        if (!immediate) {
          callback = std::thread([&] {
            fixture.submitted.wait(false, std::memory_order_acquire);
            fixture.request->notify(*fixture.request);
          });
        }
        auto result = ctx->run(weave::detail::resolve_with(*ctx, "localhost", 123, {}, fixture.api()));
        if (callback.joinable())
          callback.join();
        if (error) {
          REQUIRE_FALSE(result);
          CHECK(result.error().value() == error);
          CHECK(std::string(result.error().category().name()) == "weave.resolve");
        } else {
          REQUIRE(result);
          CHECK(*result == std::vector<weave::Endpoint>{{weave::IpAddress::loopback_v4(), 123}});
        }
        CHECK(fixture.queries == 1);
        CHECK(fixture.releases == 1);
      }
    }
  }
}

TEST_CASE("Linux resolver cancellation retains an uninterruptible query until notification")
{
  constexpr std::array stop_modes{false, true};
  for (bool context_stop : stop_modes) {
    for (int repetition = 0; repetition < 40; ++repetition) {
      auto ctx = weave::Context::create();
      REQUIRE(ctx);
      ResolverFixture fixture;
      weave::CancelSource stop;
      auto task = weave::detail::resolve_with(*ctx, "localhost", 123, {}, fixture.api());
      if (context_stop)
        weave::detail::TaskAccess::bind(task, {});
      auto job = ctx->spawn(std::move(task), {.cancel = stop.token()});
      REQUIRE(job);
      std::thread callback([&] {
        fixture.submitted.wait(false, std::memory_order_acquire);
        if (context_stop)
          ctx->request_stop();
        else
          stop.cancel();
        fixture.cancelled.wait(false, std::memory_order_acquire);
        CHECK(fixture.releases == 0);
        fixture.request->notify(*fixture.request);
      });
      auto result = ctx->run(std::move(*job).as_task());
      callback.join();
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::operation_canceled);
      CHECK(fixture.releases == 1);
    }
  }
}

TEST_CASE("Linux resolver validates numeric and invalid input before native submission")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  ResolverFixture fixture;
  constexpr std::array numeric_hosts{"127.0.0.1", "::1", "fe80::1%12"};
  constexpr std::array invalid_hosts{"", "bad host", "http://localhost", "127.1", "999.0.0.1", "::1%bad"};
  for (const char *host : numeric_hosts)
    CHECK(ctx->run(weave::detail::resolve_with(*ctx, host, 123, {}, fixture.api())));
  for (const char *host : invalid_hosts)
    CHECK_FALSE(ctx->run(weave::detail::resolve_with(*ctx, host, 123, {}, fixture.api())));
  CHECK_FALSE(ctx->run(weave::detail::resolve_with(*ctx, std::string("localhost\0junk", 14), 123, {}, fixture.api())));
  CHECK(fixture.queries == 0);

  weave::CancelSource stop;
  stop.cancel();
  auto job = ctx->spawn(
    weave::detail::resolve_with(*ctx, "localhost", 123, {}, fixture.api()),
    {.cancel = stop.token()});
  REQUIRE(job);
  auto result = ctx->run(std::move(*job).as_task());
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::operation_canceled);
  CHECK(fixture.queries == 0);
}
