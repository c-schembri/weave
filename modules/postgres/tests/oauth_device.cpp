#include "oauth_device.hpp"
#include "credential_allocations.hpp"
#include "tls_certificates.hpp"
#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <iostream>
#include <atomic>
#include <type_traits>
#include <filesystem>
#include <fstream>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

static void check(bool condition, const char *what)
{
  if (!condition) {
    std::fprintf(stderr, "Device probe: %s\n", what);
    std::_Exit(1);
  }
}

static std::span<const std::byte> bytes(std::string_view text)
{
  return std::as_bytes(std::span{text.data(), text.size()});
}

static void json_checks()
{
  const std::array invalid_grants{
    "{}",
    "[]",
    "{\"device_code\":\"x\",\"user_code\":\"x\",\"verification_uri\":\"https://host\",\"expires_in\":-1}",
    "{\"device_code\":\"x\",\"user_code\":\"x\",\"verification_uri\":\"http://host\",\"expires_in\":5}",
    "{\"device_code\":\"x\",\"user_code\":\"x\",\"verification_uri\":\"https://host\",\"expires_in\":1.5}",
    "{\"device_code\":\"x\",\"user_code\":\"x\",\"verification_uri\":\"https://"
    "host\",\"expires_in\":5,\"interval\":null}",
    "{\"device_code\":\"x\",\"user_code\":\"x\",\"verification_uri\":\"https://"
    "host\",\"expires_in\":5,\"error\":\"bad\"}",
    "{\"device_code\":\"x\",\"user_code\":\"x\",\"verification_uri\":\"https://"
    "host\",\"expires_in\":5,\"interval\":0,\"inter\\u0076al\":1}"};
  for (auto json : invalid_grants)
    check(!wire::oauth_grant(bytes(json)), "Invalid grant rejected");
  const std::array invalid_tokens{
    "{}",
    "{\"access_token\":\"abc\",\"token_type\":\"DPoP\"}",
    "{\"access_token\":\"a b\",\"token_type\":\"Bearer\"}",
    "{\"access_token\":\"abc\",\"token_type\":\"Bearer\",\"error\":\"authorization_pending\"}",
    "{\"access_token\":\"abc\",\"token_type\":\"Bearer\",\"expires_in\":-1}",
    "{\"access_token\":\"abc\",\"token_type\":\"Bearer\",\"scope\":\"a  b\"}",
    "{\"access_token\":\"abc\",\"token_type\":\"Bearer\",\"access_\\u0074oken\":\"abc\"}"};
  for (auto json : invalid_tokens)
    check(!wire::oauth_poll(bytes(json), 200), "Invalid token rejected");
  check(!pg::OAuthClientSecret::parse(""), "Empty secret rejected");
  check(!pg::OAuthClientSecret::parse(std::string_view{"a\0b", 3}), "NUL secret rejected");
  check(!pg::OAuthClientSecret::parse("\xff"), "Invalid UTF8 secret rejected");
  auto token = wire::oauth_poll(bytes("{\"access_token\":\"abc\",\"token_type\":\"bEaReR\"}"), 200);
  check(bool(token) && token->token->value() == "abc", "Case-insensitive Bearer type");
}

struct Counts {
  std::atomic<unsigned> prompts = 0;
  std::optional<fixture::CredentialAllocation> prompt_storage;
};

static pg::OAuthRequest request(std::string issuer)
{
  pg::OAuthRequest value{
    .issuer = issuer,
    .client_id = "client:/ +&=",
    .scope = "read write",
    .openid_configuration = issuer + "/.well-known/openid-configuration",
    .user = "weave",
    .database = "postgres"};
  auto mode = std::string_view{issuer}.substr(issuer.rfind('/') + 1);
  if (mode.starts_with("request_") || mode == "concurrent_secret") {
    auto parsed = pg::Options::parse("oauth_client_secret='s:e c+/&='");
    check(bool(parsed), "Parsed connection secret");
    value.client_secret = parsed->oauth->client_secret;
  }
  return value;
}

static void ownership_checks(weave::TlsContext tls)
{
  static_assert(std::is_nothrow_copy_constructible_v<pg::OAuthClientSecret>);
  static_assert(!std::is_copy_constructible_v<pg::OAuthDevicePrompt>);
  static_assert(!std::is_copy_constructible_v<wire::OAuthGrant>);
  constexpr std::string_view credential = "device-secret-owned-0123456789-device-secret-owned";
  fixture::CredentialPattern pattern{credential};
  auto first = pg::OAuthClientSecret::parse(credential);
  auto second = pg::OAuthClientSecret::parse(credential);
  check(bool(first) && bool(second), "Secret parsing");
  fixture::CredentialAllocation old{second->value()};
  *second = std::move(*first);
  check(first->value().empty() && old.cleansed(), "Secret replacement/move-source cleanup");

  auto owner = std::make_shared<Counts>();
  std::weak_ptr weak = owner;
  std::optional<weave::Task<pg::OAuthToken>> task;
  fixture::CredentialAllocation retained{second->value()};
  {
    auto provider = pg::OAuthProvider::device(
      [owner](pg::OAuthDevicePrompt) noexcept -> weave::Task<void> {
        ++owner->prompts;
        co_return;
      },
      {.tls = tls, .client_secret = std::move(*second)});
    check(bool(provider), "Credential-owning factory");
    task.emplace(provider->request(request("https://issuer.example/tenant")));
  }
  owner.reset();
  check(!weak.expired() && !retained.released, "Unstarted request owns provider and secret");
  task.reset();
  check(weak.expired() && retained.cleansed(), "Unstarted request releases and cleanses provider");
  check(pattern.dirty_releases() == 0, "No discarded dirty secret storage");

  std::optional<fixture::CredentialAllocation> device, user, uri;
  {
    auto grant = wire::oauth_grant(bytes(
      "{\"device_code\":\"device-private-owned\",\"user_code\":\"USER-CODE\","
      "\"verification_uri\":\"https://issuer.example/approve\",\"expires_in\":60}"));
    check(bool(grant), "Owning grant");
    device.emplace(std::string_view{grant->device_code.data(), grant->device_code.size()});
    user.emplace(std::string_view{grant->user_code.data(), grant->user_code.size()});
    uri.emplace(std::string_view{grant->verification_uri.data(), grant->verification_uri.size()});
    {
      auto prompt = wire::OAuthDeviceAccess::prompt(*grant, std::chrono::steady_clock::now() + 60s);
      auto moved = std::move(prompt);
      check(prompt.user_code().empty() && prompt.verification_uri().empty(), "Moved prompt is empty");
      check(moved.user_code() == "USER-CODE", "Prompt owns moved grant fields");
    }
    check(user->cleansed() && uri->cleansed() && !device->released, "Prompt excludes private device code");
  }
  check(device->cleansed(), "Private device code cleansed on grant release");

  auto handler = [](pg::OAuthDevicePrompt) noexcept -> weave::Task<void> {
    co_return;
  };
  check(!pg::OAuthProvider::device({}, {.tls = tls}), "Empty handler rejected");
  check(!pg::OAuthProvider::device(handler, {.tls = tls, .request_timeout = 0ms}), "Zero deadline rejected");
  check(!pg::OAuthProvider::device(handler, {.tls = tls, .request_timeout = 25h}), "Overlong deadline rejected");
  auto explicit_basic = pg::OAuthProvider::device(
    handler,
    {.tls = tls, .client_auth = pg::OAuthClientAuth::client_secret_basic});
  check(bool(explicit_basic), "Factory defers request-credential validation");
  auto ctx = weave::Context::create();
  check(bool(ctx), "Context for pre-network credential validation");
  auto missing = ctx->run(explicit_basic->request(request("https://issuer.example/tenant")));
  check(
    !missing && missing.error() == std::errc::invalid_argument,
    "Missing confidential credential rejected before I/O");
}

static weave::Task<void> acquire(pg::OAuthProvider provider, pg::OAuthRequest request, std::error_code expected = {})
{
  auto token = co_await weave::as_result(provider.request(std::move(request)));
  if (expected) {
    check(!token && token.error() == expected, "Expected provider failure");
  } else {
    if (!token)
      std::fprintf(stderr, "provider: %s\n", token.error().message().c_str());
    check(bool(token) && token->value() == "abc", "Native token acquisition");
  }
}

static weave::Task<void> wait_for_prompt(std::shared_ptr<Counts> counts)
{
  while (counts->prompts == 0)
    co_await weave::sleep_for(1ms);
}

static weave::Task<pg::OAuthToken> join(weave::JoinHandle<pg::OAuthToken> job)
{
  co_return co_await std::move(job);
}

#include "oauth_client_secret.hpp"

int main()
{
  json_checks();
  fixture::Certificates certificates;
  configuration_checks(certificates.ca + ".service");
  shared_owner_checks();
  std::cout << certificates.ca << '\n' << certificates.leaf << '\n' << certificates.private_key << std::endl;
  std::string issuer, mode;
  std::getline(std::cin, issuer);
  std::getline(std::cin, mode);
  auto tls = weave::TlsContext::client({.ca_file = certificates.ca, .alpn = {"http/1.1"}});
  check(bool(tls), "TLS context");
  ownership_checks(*tls);
  auto counts = std::make_shared<Counts>();
  auto handler = [counts, mode](pg::OAuthDevicePrompt prompt) noexcept -> weave::Task<void> {
    ++counts->prompts;
    check(prompt.user_code() == "TEST-1234", "Prompt user code");
    check(prompt.verification_uri().starts_with("https://"), "Prompt verification URI");
    check(!prompt.verification_uri_complete().empty(), "Prompt complete URI");
    check(prompt.expires_at() > std::chrono::steady_clock::now(), "Prompt expiry");
    if (mode == "handler_error")
      co_await weave::fail(std::errc::permission_denied);
    if (mode == "prompt_cancel" || mode == "shutdown") {
      counts->prompt_storage.emplace(prompt.user_code());
      co_await weave::sleep_for(5s);
    }
    co_await weave::sleep_for(1ms);
  };
  pg::OAuthDeviceOptions options{.tls = *tls};
  if (mode == "basic" || mode == "post") {
    auto secret = pg::OAuthClientSecret::parse("s:e c+/&=");
    check(bool(secret), "Secret owner");
    options.client_secret = std::move(*secret);
  }
  if (mode == "timeout_retry")
    options.request_timeout = 1s;
  if (mode == "request_override") {
    auto fallback = pg::OAuthClientSecret::parse("wrong-provider-default");
    check(bool(fallback), "Provider default");
    options.client_secret = std::move(*fallback);
  }
  if (mode == "request_basic" || mode == "missing_basic")
    options.client_auth = pg::OAuthClientAuth::client_secret_basic;
  if (mode == "request_post" || mode == "missing_post")
    options.client_auth = pg::OAuthClientAuth::client_secret_post;
  if (mode == "request_none")
    options.client_auth = pg::OAuthClientAuth::none;
  auto provider = pg::OAuthProvider::device(std::move(handler), std::move(options));
  check(bool(provider), "Native provider factory");
  auto ctx = weave::Context::create();
  check(bool(ctx), "Context");
  std::error_code expected;
  if (mode == "mismatch")
    expected = pg::Error::authentication;
  if (mode == "denied")
    expected = std::make_error_code(std::errc::permission_denied);
  if (mode == "handler_error")
    expected = std::make_error_code(std::errc::permission_denied);
  if (mode == "expired" || mode == "expiry" || mode == "stale")
    expected = std::make_error_code(std::errc::timed_out);
  if (mode == "request_none" || mode.starts_with("missing_"))
    expected = std::make_error_code(std::errc::invalid_argument);
  if (mode == "shutdown") {
    std::optional<weave::JoinHandle<pg::OAuthToken>> retained;
    auto job = ctx->spawn(provider->request(request(issuer)));
    check(bool(job), "Shutdown request admitted");
    retained.emplace(std::move(*job));
    check(bool(ctx->run(weave::timeout(10s, wait_for_prompt(counts)))), "Prompt starts before Context shutdown");
    ctx->shutdown();
    retained.reset();
    check(counts->prompt_storage && counts->prompt_storage->cleansed(), "Shutdown drains owning prompt");
  } else if (mode == "prompt_cancel") {
    auto job = ctx->spawn(provider->request(request(issuer)));
    check(bool(job), "Cancellable prompt admitted");
    check(bool(ctx->run(weave::timeout(10s, wait_for_prompt(counts)))), "Prompt starts before cancellation");
    job->cancel();
    auto result = ctx->run(join(std::move(*job)));
    check(!result && result.error() == std::errc::operation_canceled, "Prompt cancellation drains");
    check(counts->prompt_storage && counts->prompt_storage->cleansed(), "Cancelled prompt cleansed");
  } else if (mode == "cancel") {
    auto job = ctx->spawn(provider->request(request(issuer)));
    check(bool(job), "Cancellable polling admitted");
    check(bool(ctx->run(weave::timeout(10s, wait_for_prompt(counts)))), "Prompt starts before polling cancellation");
    check(bool(ctx->run(weave::sleep_for(1500ms))), "First polling interval runs");
    job->cancel();
    auto result = ctx->run(join(std::move(*job)));
    check(!result && result.error() == std::errc::operation_canceled, "Cancelled polling drains");
  } else if (mode == "concurrent" || mode == "concurrent_secret") {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
      check(bool(runtime), "Runtime");
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned index = 0; index < 32; ++index) {
        auto job = runtime->spawn(acquire(*provider, request(issuer)));
        check(bool(job), "Concurrent spawn");
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs)
        check(bool(std::move(job).get()), "Concurrent acquisition");
      runtime->join();
    }
#else
    check(false, "Concurrency mode requires Runtime");
#endif
  } else {
    auto result = ctx->run(weave::timeout(30s, acquire(*provider, request(issuer), expected)));
    check(bool(result), "Provider graph completion");
  }
  unsigned expected_prompts = 1;
  if (mode == "mismatch" || mode == "stale" || mode == "request_none" || mode.starts_with("missing_"))
    expected_prompts = 0;
  if (mode == "concurrent" || mode == "concurrent_secret")
    expected_prompts = 64;
  check(counts->prompts == expected_prompts, "Prompt count");
  std::cout << "native device provider passed " << mode << " prompts=" << counts->prompts << std::endl;
}
