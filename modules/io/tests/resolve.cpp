#include "windows/resolve.hpp"
#include <doctest/doctest.h>
#include <array>
#include <atomic>
#include <thread>

struct ResolverFixture {
  enum class Mode {
    immediate,
    pending,
    inline_completion,
    error,
    pending_error,
    empty,
    cancel
  };
  Mode mode;
  weave::CancelSource *stop = nullptr;
  bool stop_before_registration = false;
  std::atomic<bool> submitted = false;
  std::atomic<bool> cancelled = false;
  std::atomic<unsigned> queries = 0;
  std::atomic<unsigned> releases = 0;
  std::atomic<unsigned> cancels = 0;
  LPWSAOVERLAPPED operation = nullptr;
  LPLOOKUPSERVICE_COMPLETION_ROUTINE completion = nullptr;
  std::array<ADDRINFOEXW, 3> results{};
  std::array<weave::detail::SocketAddress, 3> addresses{};
  static inline ResolverFixture *active = nullptr;

  explicit ResolverFixture(Mode value) : mode(value)
  {
    active = this;
    for (std::size_t i = 0; i < results.size(); ++i) {
      addresses[i] = weave::detail::socket_address(
        {i == 2 ? weave::IpAddress::loopback_v6() : weave::IpAddress::loopback_v4(), 123});
      results[i].ai_addr = addresses[i].data();
      results[i].ai_addrlen = addresses[i].size;
      results[i].ai_family = addresses[i].data()->sa_family;
      results[i].ai_next = i + 1 < results.size() ? &results[i + 1] : nullptr;
    }
  }

  ~ResolverFixture()
  {
    active = nullptr;
  }

  static int WSAAPI query(
    PCWSTR,
    PCWSTR,
    DWORD,
    LPGUID,
    const ADDRINFOEXW *,
    PADDRINFOEXW *result,
    timeval *,
    LPOVERLAPPED operation,
    LPLOOKUPSERVICE_COMPLETION_ROUTINE completion,
    LPHANDLE handle)
  {
    auto &fixture = *active;
    ++fixture.queries;
    if (fixture.mode == Mode::error)
      return WSAHOST_NOT_FOUND;
    if (fixture.mode == Mode::empty)
      return 0;
    *result = fixture.results.data();
    if (fixture.mode == Mode::immediate)
      return 0;
    fixture.operation = operation;
    fixture.completion = completion;
    *handle = reinterpret_cast<HANDLE>(1);
    if (fixture.stop_before_registration)
      fixture.stop->cancel();
    fixture.submitted.store(true, std::memory_order_release);
    fixture.submitted.notify_one();
    if (fixture.mode == Mode::inline_completion)
      completion(0, 0, operation);
    return WSA_IO_PENDING;
  }

  static int WSAAPI cancel(LPHANDLE handle)
  {
    auto &fixture = *active;
    ++fixture.cancels;
    CHECK(fixture.releases.load() == 0);
    *handle = nullptr;
    fixture.cancelled.store(true, std::memory_order_release);
    fixture.cancelled.notify_one();
    return 0;
  }

  static void WSAAPI release(PADDRINFOEXW result)
  {
    CHECK(result == active->results.data());
    ++active->releases;
  }

  weave::Task<std::vector<weave::Endpoint>> task(weave::Context &ctx, std::string host = "localhost")
  {
    return weave::detail::resolve_with(ctx, std::move(host), 123, {}, {query, cancel, release});
  }
};

TEST_CASE("Resolver numeric fast paths and invalid names do not submit DNS")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  ResolverFixture fixture(ResolverFixture::Mode::error);
  constexpr std::array numeric_hosts{"127.0.0.1", "::1", "fe80::1%12"};
  constexpr std::array invalid_hosts{"", "http://localhost", "bad host", ":::1", "999.0.0.1", "127.1", "::1%bad"};

  for (const char *host : numeric_hosts) {
    auto result = ctx->run(fixture.task(*ctx, host));
    REQUIRE(result);
    REQUIRE(result->size() == 1);
    CHECK(result->front().address == *weave::IpAddress::parse(host));
  }
  for (const char *host : invalid_hosts) {
    auto result = ctx->run(fixture.task(*ctx, host));
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::invalid_argument);
  }
  CHECK_FALSE(ctx->run(fixture.task(*ctx, std::string("localhost\0junk", 14))));
  CHECK_FALSE(ctx->run(weave::resolve(*ctx, "::1", 123, {.family = weave::AddressFamily::v4})));
  CHECK_FALSE(ctx->run(weave::resolve(*ctx, "localhost", 123, {.family = static_cast<weave::AddressFamily>(9)})));
  CHECK(fixture.queries == 0);
  CHECK_FALSE(ctx->run(fixture.task(*ctx, std::string(1, static_cast<char>(0xff)))));
  CHECK_FALSE(ctx->run(fixture.task(*ctx, std::string(65536, 'a'))));
  CHECK(fixture.queries == 0);
}

TEST_CASE("Resolver handles immediate, pending and early completion, deduplicating in system order")
{
  constexpr std::array completion_modes{
    ResolverFixture::Mode::immediate,
    ResolverFixture::Mode::pending,
    ResolverFixture::Mode::inline_completion,
    ResolverFixture::Mode::error,
    ResolverFixture::Mode::pending_error,
    ResolverFixture::Mode::empty};

  for (auto mode : completion_modes) {
    for (int repetition = 0; repetition < 20; ++repetition) {
      auto ctx = weave::Context::create();
      REQUIRE(ctx);
      ResolverFixture fixture(mode);
      std::thread callback;
      if (mode == ResolverFixture::Mode::pending || mode == ResolverFixture::Mode::pending_error) {
        callback = std::thread([&] {
          fixture.submitted.wait(false, std::memory_order_acquire);
          fixture.completion(
            mode == ResolverFixture::Mode::pending_error ? WSAHOST_NOT_FOUND : 0,
            0,
            fixture.operation);
        });
      }
      auto result = ctx->run(fixture.task(*ctx));
      if (callback.joinable())
        callback.join();
      if (mode == ResolverFixture::Mode::error || mode == ResolverFixture::Mode::pending_error ||
        mode == ResolverFixture::Mode::empty) {
        REQUIRE_FALSE(result);
        CHECK(result.error().value() == (mode == ResolverFixture::Mode::empty ? WSANO_DATA : WSAHOST_NOT_FOUND));
        CHECK(fixture.releases == (mode == ResolverFixture::Mode::pending_error ? 1 : 0));
      } else {
        REQUIRE(result);
        REQUIRE(result->size() == 2);
        CHECK(result->at(0) == weave::Endpoint{weave::IpAddress::loopback_v4(), 123});
        CHECK(result->at(1) == weave::Endpoint{weave::IpAddress::loopback_v6(), 123});
        CHECK(fixture.releases == 1);
      }
      CHECK(fixture.queries == 1);
      CHECK(fixture.cancels == 0);
    }
  }
}

TEST_CASE("Resolver cancellation drains completion before releasing native results")
{
  for (int mode = 0; mode < 3; ++mode) {
    for (int repetition = 0; repetition < 20; ++repetition) {
      auto ctx = weave::Context::create();
      REQUIRE(ctx);
      weave::CancelSource stop;
      ResolverFixture fixture(ResolverFixture::Mode::cancel);
      fixture.stop = &stop;
      fixture.stop_before_registration = mode == 2;
      auto task = fixture.task(*ctx);
      if (mode == 1)
        weave::detail::TaskAccess::bind(task, {});
      auto job = ctx->spawn(std::move(task), {.cancel = stop.token()});
      REQUIRE(job);
      std::thread callback([&] {
        fixture.submitted.wait(false, std::memory_order_acquire);
        if (mode == 1)
          ctx->request_stop();
        else
          stop.cancel();
        fixture.cancelled.wait(false, std::memory_order_acquire);
        CHECK(fixture.releases.load() == 0);
        fixture.completion(WSA_E_CANCELLED, 0, fixture.operation);
      });
      auto result = ctx->run(std::move(*job).as_task());
      callback.join();
      REQUIRE_FALSE(result);
      CHECK(result.error() == std::errc::operation_canceled);
      CHECK(fixture.cancels == 1);
      CHECK(fixture.releases == 1);
    }
  }
}

TEST_CASE("Pre-cancelled resolver tasks do not start native queries")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  weave::CancelSource stop;
  stop.cancel();
  ResolverFixture fixture(ResolverFixture::Mode::error);
  constexpr std::array hosts{"localhost", "::1"};

  for (const char *host : hosts) {
    auto job = ctx->spawn(fixture.task(*ctx, host), {.cancel = stop.token()});
    REQUIRE(job);
    auto result = ctx->run(std::move(*job).as_task());
    REQUIRE_FALSE(result);
    CHECK(result.error() == std::errc::operation_canceled);
  }
  CHECK(fixture.queries == 0);
}

TEST_CASE("Native async resolver supports local names and family filters without external DNS")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  constexpr std::array families{weave::AddressFamily::any, weave::AddressFamily::v4, weave::AddressFamily::v6};

  for (auto family : families) {
    auto result = ctx->run(weave::resolve(std::string("localhost"), 8080, {.family = family}));
    REQUIRE(result);
    REQUIRE_FALSE(result->empty());
    for (auto endpoint : *result) {
      CHECK(endpoint.port == 8080);
      if (family != weave::AddressFamily::any)
        CHECK(endpoint.address.family() == family);
    }
  }
}
