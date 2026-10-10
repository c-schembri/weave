#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <weave/postgres/oauth.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include "oauth.hpp"
#include "credential_allocations.hpp"
#include <atomic>
#include <optional>
#include <type_traits>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

static std::span<const std::byte> bytes(std::string_view text)
{
  return std::as_bytes(std::span{text.data(), text.size()});
}

static constexpr std::string_view advertised{"OAUTHBEARER\0\0", 13};
static constexpr std::string_view credential = "token-owned-0123456789-token-owned-0123456789";

TEST_CASE("Bearer tokens validate the complete bounded RFC6750 grammar")
{
  static_assert(!std::is_copy_constructible_v<pg::OAuthToken>);
  static_assert(std::is_nothrow_move_constructible_v<pg::OAuthToken>);
  static_assert(std::is_copy_constructible_v<pg::OAuthProvider>);
  const std::array valid{"a", "a=", "A.Z_-~+/09===", "jwt.header.signature"};
  for (auto value : valid) {
    auto token = pg::OAuthToken::parse(value);
    REQUIRE(token);
    CHECK(token->value() == value);
  }

  const std::array invalid{"", "=", "==a", "a=b", "a b", "a\t", "a\r\n", "a\x01", "a\x7f", "a\xff"};
  for (auto value : invalid) {
    auto token = pg::OAuthToken::parse(value);
    CHECK((!token && token.error() == std::errc::invalid_argument));
  }
  CHECK_FALSE(pg::OAuthToken::parse(std::string_view{"a\0b", 3}));
  CHECK(pg::OAuthToken::parse(std::string(65536, 'a')));
  CHECK_FALSE(pg::OAuthToken::parse(std::string(65537, 'a')));
}

TEST_CASE("OAuth wire bytes agree with an independent byte oracle and legal state transitions")
{
  auto token = pg::OAuthToken::parse("abc+/==");
  REQUIRE(token);
  wire::OAuthExchange exchange;
  CHECK_FALSE(exchange.complete());
  CHECK_FALSE(exchange.accept());
  CHECK_FALSE(exchange.reject());
  auto response = exchange.start(bytes(advertised), *token);
  REQUIRE(response);

  // Expected framing is literal, not built by the writer under test.
  constexpr std::string_view expected{
    "OAUTHBEARER\0\0\0\0\x19"
    "n,,\x01"
    "auth=Bearer abc+/==\x01\x01",
    41};
  CHECK(std::ranges::equal(response->bytes, bytes(expected)));
  CHECK_FALSE(exchange.start(bytes(advertised), *token));
  CHECK(exchange.accept());
  CHECK(exchange.complete());
  CHECK_FALSE(exchange.accept());
  CHECK_FALSE(exchange.reject());

  wire::OAuthExchange rejected;
  REQUIRE(rejected.start(bytes(advertised), *token));
  auto dummy = rejected.reject();
  REQUIRE(dummy);
  REQUIRE(dummy->bytes.size() == 1);
  CHECK(dummy->bytes.front() == std::byte{1});
  CHECK_FALSE(rejected.accept());
  CHECK_FALSE(rejected.reject());
  CHECK_FALSE(rejected.complete());
}

TEST_CASE("OAuth mechanism lists require exact termination and bounded admission")
{
  auto token = pg::OAuthToken::parse("abc");
  REQUIRE(token);
  const std::array malformed{
    std::string{},
    std::string{"OAUTHBEARER"},
    std::string{"OAUTHBEARER\0", 12},
    std::string{"OAUTHBEARER\0\0trailing", 21},
    std::string{"OAUTHBEARER\0OAUTHBEARER\0\0", 25}};
  for (const auto &list : malformed) {
    wire::OAuthExchange exchange;
    auto result = exchange.start(bytes(list), *token);
    CHECK((!result && result.error() == pg::Error::protocol));
  }
  wire::OAuthExchange unsupported;
  auto missing = unsupported.start(bytes(std::string_view{"SCRAM-SHA-256\0\0", 15}), *token);
  CHECK((!missing && missing.error() == pg::Error::unsupported_authentication));

  const std::array sizes{256u, 257u, 65536u};
  for (auto size : sizes) {
    auto list = std::string(size, 'x');
    list.push_back('\0');
    list.append(advertised);
    wire::OAuthExchange exchange;
    auto result = exchange.start(bytes(list), *token);
    CHECK(bool(result) == (size == 256));
    if (!result)
      CHECK(result.error() == pg::Error::resource_limit);
  }
  const std::array counts{63u, 64u};
  for (auto count : counts) {
    std::string list;
    for (unsigned index = 0; index < count; ++index)
      list.append("x\0", 2);
    list.append(advertised);
    wire::OAuthExchange exchange;
    auto result = exchange.start(bytes(list), *token);
    CHECK(bool(result) == (count == 63));
    if (!result)
      CHECK(result.error() == pg::Error::resource_limit);
  }
}

TEST_CASE("OAuth token moves and grown wire packets cleanse every discarded owned allocation")
{
  fixture::CredentialPattern pattern{credential};
  auto first = pg::OAuthToken::parse(credential);
  auto second = pg::OAuthToken::parse(credential);
  REQUIRE(first);
  REQUIRE(second);
  fixture::CredentialAllocation replaced{second->value()};
  fixture::CredentialAllocation moved{first->value()};
  *second = std::move(*first);
  CHECK(first->value().empty());
  CHECK(replaced.cleansed());
  {
    auto token = std::move(*second);
    CHECK(second->value().empty());
    wire::OAuthExchange exchange;
    auto response = exchange.start(bytes(advertised), token);
    REQUIRE(response);
    fixture::CredentialAllocation old_capacity{
      {reinterpret_cast<const char *>(response->bytes.data()), response->bytes.size()}};
    response->bytes.reserve(response->bytes.capacity() + 1);
    CHECK(old_capacity.cleansed());
  }
  CHECK(moved.cleansed());
  CHECK(pattern.dirty_releases() == 0);
}

struct Owner {
  std::atomic<unsigned> calls{0};
  std::atomic<unsigned> completions{0};
};

static weave::Task<pg::OAuthToken> join(weave::JoinHandle<pg::OAuthToken> job)
{
  co_return co_await std::move(job);
}

static weave::Task<pg::OAuthToken> deferred(
  std::shared_ptr<Owner> owner,
  bool fail = false,
  std::chrono::milliseconds delay = 5ms)
{
  auto provider = pg::OAuthProvider::create(
    [owner = std::move(owner), fail, delay](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
      ++owner->calls;
      co_await weave::sleep_for(delay);
      if (request.user != "user" || request.database != "database")
        co_await weave::fail(std::errc::invalid_argument);
      if (fail)
        co_await weave::fail(std::errc::permission_denied);
      ++owner->completions;
      auto token = pg::OAuthToken::parse(credential);
      if (!token)
        co_await weave::fail(token.error());
      co_return std::move(*token);
    });
  weave::detail::require(bool(provider));
  return provider->request({.user = "user", .database = "database"});
}

TEST_CASE("Provider owns coroutine lambda closure before suspension and releases every outcome")
{
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto owner = std::make_shared<Owner>();
  std::weak_ptr weak = owner;
  {
    auto unstarted = deferred(owner);
    owner.reset();
    CHECK_FALSE(weak.expired());
  }
  CHECK(weak.expired());

  const std::array failures{false, true};
  for (bool failure : failures) {
    owner = std::make_shared<Owner>();
    weak = owner;
    auto task = deferred(owner, failure);
    owner.reset();
    auto result = ctx->run(std::move(task));
    CHECK(bool(result) == !failure);
    if (result)
      CHECK(result->value() == credential);
    else
      CHECK(result.error() == std::errc::permission_denied);
    CHECK(weak.expired());
  }

  owner = std::make_shared<Owner>();
  weak = owner;
  auto result = ctx->run(weave::timeout(1ms, deferred(owner, false, 1h)));
  REQUIRE_FALSE(result);
  CHECK(result.error() == std::errc::timed_out);
  CHECK(owner->calls == 1);
  CHECK(owner->completions == 0);
  owner.reset();
  CHECK(weak.expired());
}

TEST_CASE("Provider rejects invalid requests and cancelled admission before invoking user code")
{
  CHECK_FALSE(pg::OAuthProvider::create({}));
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto owner = std::make_shared<Owner>();
  auto provider = pg::OAuthProvider::create([owner](pg::OAuthRequest) noexcept -> weave::Task<pg::OAuthToken> {
    ++owner->calls;
    co_return std::move(*pg::OAuthToken::parse("abc"));
  });
  REQUIRE(provider);
  const std::array fields{
    &pg::OAuthRequest::issuer,
    &pg::OAuthRequest::client_id,
    &pg::OAuthRequest::scope,
    &pg::OAuthRequest::openid_configuration,
    &pg::OAuthRequest::host,
    &pg::OAuthRequest::user,
    &pg::OAuthRequest::database};
  const std::array invalid_values{std::string(65537, 'a'), std::string{"a\0b", 3}};
  for (auto field : fields) {
    for (const auto &value : invalid_values) {
      pg::OAuthRequest request;
      request.*field = value;
      auto invalid = ctx->run(provider->request(std::move(request)));
      CHECK((!invalid && invalid.error() == std::errc::invalid_argument));
    }
  }
  CHECK(owner->calls == 0);

  weave::CancelSource cancel;
  cancel.cancel();
  auto job = ctx->spawn(provider->request({}), {.cancel = cancel.token()});
  REQUIRE(job);
  auto cancelled = ctx->run(join(std::move(*job)));
  CHECK((!cancelled && cancelled.error() == std::errc::operation_canceled));
  CHECK(owner->calls == 0);
}

TEST_CASE("Provider ownership survives rejection and shutdown while pending")
{
  auto owner = std::make_shared<Owner>();
  std::weak_ptr weak = owner;
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  ctx->shutdown();
  auto rejected = ctx->spawn(deferred(owner));
  CHECK_FALSE(rejected);
  CHECK(owner->calls == 0);
  owner.reset();
  CHECK(weak.expired());

  auto active = weave::Context::create();
  REQUIRE(active);
  owner = std::make_shared<Owner>();
  weak = owner;
  auto job = active->spawn(deferred(owner, false, 1h));
  REQUIRE(job);
  REQUIRE(active->run(weave::sleep_for(1ms)));
  CHECK(owner->calls == 1);
  active->shutdown();
  CHECK(owner->completions == 0);
  owner.reset();
  CHECK(weak.expired());
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
TEST_CASE("Thirty two independent providers drain on four workers under both schedulers")
{
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    auto owner = std::make_shared<Owner>();
    std::vector<weave::JoinHandle<pg::OAuthToken>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(deferred(owner));
      REQUIRE(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      REQUIRE(result);
      CHECK(result->value() == credential);
    }
    runtime->join();
    CHECK(owner->calls == 32);
    CHECK(owner->completions == 32);
    CHECK(owner.use_count() == 1);
  }
}

TEST_CASE("One const callable provider safely serves concurrent owning requests and cancelled children")
{
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  const std::array cancelled{false, true};
  for (auto scheduler : schedulers) {
    for (bool cancel : cancelled) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
      REQUIRE(runtime);
      auto owner = std::make_shared<Owner>();
      std::weak_ptr weak = owner;
      auto provider = pg::OAuthProvider::create(
        [owner, cancel](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
          ++owner->calls;
          co_await weave::sleep_for(cancel ? 1h : 5ms);
          if (request.host != "configured.example" || request.client_id != "client" || request.port != 5432)
            co_await weave::fail(std::errc::invalid_argument);
          ++owner->completions;
          auto token = pg::OAuthToken::parse(credential);
          if (!token)
            co_await weave::fail(token.error());
          co_return std::move(*token);
        });
      REQUIRE(provider);

      std::vector<weave::JoinHandle<pg::OAuthToken>> jobs;
      for (unsigned index = 0; index < 32; ++index) {
        auto task = provider->request({.client_id = "client", .host = "configured.example", .port = 5432});
        if (cancel) {
          auto job = runtime->spawn(weave::timeout(50ms, std::move(task)));
          REQUIRE(job);
          jobs.push_back(std::move(*job));
        } else {
          auto job = runtime->spawn(std::move(task));
          REQUIRE(job);
          jobs.push_back(std::move(*job));
        }
      }
      provider = std::unexpected(std::make_error_code(std::errc::operation_canceled));
      for (auto &job : jobs) {
        auto result = std::move(job).get();
        CHECK(bool(result) == !cancel);
        if (!result)
          CHECK(result.error() == std::errc::timed_out);
      }
      runtime->join();
      CHECK(owner->calls == 32);
      CHECK(owner->completions == (cancel ? 0u : 32u));
      CHECK(owner.use_count() == 1);
      owner.reset();
      CHECK(weak.expired());
    }
  }
}
#endif

TEST_CASE("Failed or cancelled provider frames cleanse tokens constructed before suspension")
{
  const std::array failures{false, true};
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  fixture::CredentialPattern pattern{credential};
  for (bool failure : failures) {
    auto provider = pg::OAuthProvider::create([failure](pg::OAuthRequest) noexcept -> weave::Task<pg::OAuthToken> {
      auto token = pg::OAuthToken::parse(credential);
      if (!token)
        co_await weave::fail(token.error());
      if (failure)
        co_await weave::fail(std::errc::permission_denied);
      co_await weave::sleep_for(1h);
      co_return std::move(*token);
    });
    REQUIRE(provider);
    auto result = ctx->run(weave::timeout(1ms, provider->request({})));
    CHECK((!result && result.error() == (failure ? std::errc::permission_denied : std::errc::timed_out)));
  }
  CHECK(pattern.dirty_releases() == 0);
}
