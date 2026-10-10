#pragma once

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif

TEST_CASE("Explicit discovery identifiers derive literal issuers and pin server metadata")
{
  struct Identity {
    const char *url;
    const char *issuer;
  };

  const std::array identities{
    Identity{"https://issuer.example/.well-known/openid-configuration", "https://issuer.example"},
    Identity{"https://issuer.example/tenant/.well-known/openid-configuration", "https://issuer.example/tenant"},
    Identity{"https://issuer.example/.well-known/openid-configuration/tenant", "https://issuer.example/tenant"},
    Identity{"https://issuer.example/.well-known/oauth-authorization-server/tenant", "https://issuer.example/tenant"},
    Identity{"https://issuer.example/tenant/.well-known/oauth-authorization-server", "https://issuer.example/tenant"},
    Identity{"https://issuer.example/.well-known/openid-configuration/", "https://issuer.example/"},
    Identity{
      "https://issuer.example:8443/.well-known/openid-configuration/a%20b",
      "https://issuer.example:8443/a%20b"}};
  for (auto item : identities) {
    auto identity = wire::oauth_identity(item.url);
    REQUIRE(identity);
    CHECK(identity->issuer == item.issuer);
    CHECK(identity->openid_configuration == item.url);
    auto options = policy();
    options.issuer = item.url;
    CHECK(wire::valid_oauth_options(options));
    auto missing = wire::oauth_discovery(bytes(R"({"status":"invalid_token"})"), options);
    REQUIRE(missing);
    CHECK(missing->openid_configuration == item.url);
    auto matching = std::string{R"({"status":"invalid_token","openid-configuration":")"} + item.url + "\"}";
    REQUIRE(wire::oauth_discovery(bytes(matching), options));
    auto different = std::string{R"({"status":"invalid_token","openid-configuration":")"} + item.issuer +
      "/.well-known/openid-configuration/other\"}";
    auto rejected = wire::oauth_discovery(bytes(different), options);
    CHECK((!rejected && rejected.error() == pg::Error::authentication));
  }
  const std::array invalid{
    "https://issuer.example/.well-known/unknown",
    "https://issuer.example/.well-known/openid-configuration-bad",
    "https://issuer.example/a/.well-known/openid-configuration/b",
    "https://issuer.example/.well-known/openid-configuration/.well-known/openid-configuration",
    "https://issuer.example/.well-known/openid-configuration?query",
    "https://issuer.example/.well-known/openid-configuration#fragment"};
  for (auto value : invalid)
    CHECK_FALSE(wire::oauth_identity(value));
}

enum class CachedMode {
  hit,
  absent_scope,
  empty_scope,
  miss,
  literal_issuer,
  error,
  moved_token,
  rejected,
  rejected_success,
  retarget,
  binding,
  other_method,
  no_challenge,
  cancelled
};

struct CachedCounts {
  Counts flow;
  unsigned lookups = 0;
  weave::CancelSource cancellation;
  std::optional<fixture::CredentialAllocation> token;
};

static bool falls_back(CachedMode mode)
{
  return mode == CachedMode::miss || mode == CachedMode::literal_issuer;
}

static weave::Task<void> cached_peer(
  weave::TcpListener &listener,
  weave::TlsContext &tls,
  CachedCounts &counts,
  CachedMode mode)
{
  if (falls_back(mode)) {
    co_await peer(listener, tls, counts.flow, Mode::normal);
    co_return;
  }
  auto socket = co_await listener.accept();
  ++counts.flow.connections;
  std::array<std::byte, 8> ssl;
  co_await socket.read_exactly(ssl);
  wire::Reader reader{ssl};
  if (reader.integer() != 8 || reader.integer() != 80877103)
    co_await weave::fail(std::errc::bad_message);
  constexpr std::array selected{std::byte{'S'}};
  co_await socket.write_all(selected);
  auto stream = co_await weave::tls::server(std::move(socket), tls);
  if (mode == CachedMode::other_method || mode == CachedMode::no_challenge) {
    std::array<std::byte, 4> length;
    co_await stream.read_exactly(length);
    wire::Reader size{length};
    wire::Bytes startup(size.integer() - 4);
    co_await stream.read_exactly(startup);
    co_await send_auth(stream, mode == CachedMode::other_method ? 3 : 0);
    co_await expect_closed(stream);
    co_return;
  }
  co_await authenticate(stream);
  bool before_response = mode == CachedMode::error || mode == CachedMode::moved_token || mode == CachedMode::binding ||
    mode == CachedMode::cancelled;
  if (before_response) {
    co_await expect_closed(stream);
    co_return;
  }
  auto initial = co_await receive(stream);
  constexpr std::string_view token{
    "OAUTHBEARER\0\0\0\0\x15"
    "n,,\x01"
    "auth=Bearer abc\x01\x01",
    37};
  if (initial.first != 'p' || !std::ranges::equal(initial.second, bytes(token)))
    co_await weave::fail(std::errc::bad_message);
  if (mode == CachedMode::rejected || mode == CachedMode::rejected_success || mode == CachedMode::retarget) {
    auto discovery = mode == CachedMode::retarget
      ? R"({"status":"invalid_token","openid-configuration":"https://attacker.example/.well-known/openid-configuration"})"
      : R"({"status":"invalid_token","openid-configuration":"https://issuer.example/tenant/.well-known/openid-configuration"})";
    co_await send_auth(stream, 11, discovery);
    if (mode == CachedMode::retarget) {
      co_await expect_closed(stream);
      co_return;
    }
    auto dummy = co_await receive(stream);
    if (dummy.first != 'p' || dummy.second != wire::Bytes{std::byte{1}})
      co_await weave::fail(std::errc::bad_message);
    if (mode == CachedMode::rejected_success) {
      co_await send_auth(stream, 0);
    } else {
      wire::Writer diagnostic;
      diagnostic.integer('C', 1);
      diagnostic.string("28000");
      diagnostic.integer('M', 1);
      diagnostic.string("token rejected");
      diagnostic.integer(0, 1);
      wire::Writer packet;
      packet.message('E', diagnostic);
      co_await stream.write_all(packet.bytes);
    }
    co_await expect_closed(stream);
    co_return;
  }
  wire::Writer success;
  success.integer(0);
  wire::Writer ready;
  ready.integer('I', 1);
  wire::Writer packet;
  packet.message('R', success);
  packet.message('Z', ready);
  co_await stream.write_all(packet.bytes);
  auto terminated = co_await receive(stream);
  if (terminated.first != 'X')
    co_await weave::fail(std::errc::bad_message);
  static_cast<void>(co_await weave::as_result(stream.shutdown()));
}

static weave::Task<void> cached_scenario(
  weave::Context &ctx,
  weave::TlsContext client_tls,
  weave::TlsContext server_tls,
  CachedMode mode)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  auto fallback = co_await weave::tcp::listen("127.0.0.1", 0);
  auto counts = std::make_shared<CachedCounts>();
  auto provider = pg::OAuthProvider::create(
    [counts](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
      ++counts->flow.callbacks;
      co_await weave::sleep_for(20ms);
      if (!counts->flow.discovery_closed || request.issuer != "https://issuer.example/tenant" ||
        request.scope != "read write")
        co_await weave::fail(std::errc::bad_message);
      auto token = pg::OAuthToken::parse("abc");
      co_return std::move(*token);
    },
    [counts, mode](const pg::OAuthRequest &request) noexcept -> weave::Result<std::optional<pg::OAuthToken>> {
      ++counts->lookups;
      bool explicit_scope = mode != CachedMode::absent_scope;
      auto scope = mode == CachedMode::absent_scope || mode == CachedMode::empty_scope ? "" : "read write";
      if (request.issuer != "https://issuer.example/tenant" ||
        request.openid_configuration != "https://issuer.example/tenant/.well-known/openid-configuration" ||
        request.scope != scope || request.scope_explicit != explicit_scope || request.user != "user" ||
        request.database != "user" || !request.client_secret || request.client_secret->value() != "configured-secret")
        return std::unexpected(std::make_error_code(std::errc::bad_message));
      if (mode == CachedMode::miss)
        return std::optional<pg::OAuthToken>{};
      if (mode == CachedMode::error)
        return std::unexpected(std::make_error_code(std::errc::permission_denied));
      auto token = pg::OAuthToken::parse("abc");
      counts->token.emplace(token->value());
      if (mode == CachedMode::moved_token) {
        auto owner = std::move(*token);
        return std::optional{std::move(*token)};
      }
      if (mode == CachedMode::cancelled)
        counts->cancellation.cancel();
      return std::optional{std::move(*token)};
    });
  if (!provider)
    co_await weave::fail(provider.error());
  pg::Options options;
  options.host = "127.0.0.1";
  options.port = listener.local_port();
  options.hosts = {{.name = options.host, .port = options.port}, {.name = options.host, .port = fallback.local_port()}};
  options.user = "user";
  options.tls = client_tls;
  options.authentication.methods = {pg::Authentication::oauth};
  options.connect_timeout = 2s;
  options.oauth = policy();
  if (mode != CachedMode::literal_issuer)
    options.oauth->issuer += "/.well-known/openid-configuration";
  if (mode != CachedMode::absent_scope)
    options.oauth->scope = mode == CachedMode::empty_scope ? "" : "read write";
  options.oauth->provider = *provider;
  auto secret = pg::OAuthClientSecret::parse("configured-secret");
  options.oauth->client_secret = std::move(*secret);
  if (mode == CachedMode::binding)
    options.channel_binding = pg::ChannelBinding::require;
  auto client = [options = std::move(options), mode]() mutable -> weave::Task<void> {
    auto result = co_await weave::as_result(pg::connect(std::move(options)));
    bool success = mode == CachedMode::hit || mode == CachedMode::absent_scope || mode == CachedMode::empty_scope ||
      falls_back(mode);
    if (success) {
      if (!result)
        co_await weave::fail(result.error());
      co_await result->finish();
      co_return;
    }
    std::error_code expected = pg::Error::authentication;
    if (mode == CachedMode::error)
      expected = std::make_error_code(std::errc::permission_denied);
    if (mode == CachedMode::rejected)
      expected = pg::sql_error("28000");
    if (mode == CachedMode::other_method)
      expected = pg::Error::authentication;
    if (mode == CachedMode::cancelled)
      expected = std::make_error_code(std::errc::operation_canceled);
    if (result || result.error() != expected)
      co_await weave::fail(std::errc::bad_message);
  };
  auto job = ctx.spawn(client(), {.cancel = counts->cancellation.token()});
  if (!job)
    co_await weave::fail(job.error());
  auto joined = [&job]() -> weave::Task<void> {
    co_await std::move(*job);
  };
  co_await weave::when_all(cached_peer(listener, server_tls, *counts, mode), joined());
  bool no_lookup = mode == CachedMode::literal_issuer || mode == CachedMode::binding ||
    mode == CachedMode::other_method || mode == CachedMode::no_challenge;
  if (counts->lookups != (no_lookup ? 0u : 1u) || counts->flow.callbacks != (falls_back(mode) ? 1u : 0u) ||
    counts->flow.connections != (falls_back(mode) ? 2u : 1u))
    co_await weave::fail(std::errc::bad_message);
  if (counts->token && !counts->token->cleansed())
    co_await weave::fail(std::errc::bad_message);
  auto unused = co_await weave::as_result(weave::timeout(5ms, fallback.accept()));
  if (unused || unused.error() != std::errc::timed_out)
    co_await weave::fail(std::errc::bad_message);
}

TEST_CASE("Cached tokens use one protected connection; misses discover and errors never refresh or fail over")
{
  fixture::Certificates certificates;
  auto tls = weave::TlsContext::client({.ca_file = certificates.ca});
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  REQUIRE(tls);
  REQUIRE(server);
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  const std::array modes{
    CachedMode::hit,
    CachedMode::absent_scope,
    CachedMode::empty_scope,
    CachedMode::miss,
    CachedMode::literal_issuer,
    CachedMode::error,
    CachedMode::moved_token,
    CachedMode::rejected,
    CachedMode::rejected_success,
    CachedMode::retarget,
    CachedMode::binding,
    CachedMode::other_method,
    CachedMode::no_challenge,
    CachedMode::cancelled};
  for (auto mode : modes) {
    auto result = ctx->run(weave::timeout(10s, cached_scenario(*ctx, *tls, *server, mode)));
    INFO(static_cast<int>(mode));
    auto diagnostic = result ? std::string{"success"} : result.error().message();
    INFO(diagnostic);
    REQUIRE(result);
  }
}

TEST_CASE("Cache callables survive reentrant owner release and deferred connect destruction cleans credentials")
{
  auto lifetime = std::make_shared<unsigned>(0);
  std::weak_ptr weak = lifetime;
  std::optional<pg::OAuthProvider> owner;
  auto provider = pg::OAuthProvider::create(
    [](pg::OAuthRequest) noexcept -> weave::Task<pg::OAuthToken> {
      co_await weave::fail(std::errc::permission_denied);
      std::abort();
    },
    [retained = lifetime, &owner](const pg::OAuthRequest &) noexcept -> weave::Result<std::optional<pg::OAuthToken>> {
      owner.reset();
      ++*retained;
      auto token = pg::OAuthToken::parse("abc");
      return std::optional{std::move(*token)};
    });
  REQUIRE(provider);
  owner.emplace(std::move(*provider));
  lifetime.reset();
  CHECK_FALSE(weak.expired());
  auto result = owner->cached_token({});
  REQUIRE(result);
  REQUIRE(*result);
  CHECK((*result)->value() == "abc");
  CHECK(weak.expired());

  auto options = pg::Options::parse("oauth_client_secret=deferred-cache-secret-owned-0123456789");
  REQUIRE(options);
  fixture::CredentialAllocation secret{options->oauth->client_secret->value()};
  {
    auto deferred = pg::connect(std::move(*options));
    CHECK_FALSE(secret.released);
  }
  CHECK(secret.cleansed());
}

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
TEST_CASE("Single-connection cached sessions drain on four workers with either scheduler")
{
  fixture::Certificates certificates;
  auto tls = weave::TlsContext::client({.ca_file = certificates.ca});
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  REQUIRE(tls);
  REQUIRE(server);
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn([&](weave::Context &ctx) {
        return weave::timeout(10s, cached_scenario(ctx, *tls, *server, CachedMode::hit));
      });
      REQUIRE(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      auto diagnostic = result ? std::string{"success"} : result.error().message();
      INFO(diagnostic);
      REQUIRE(result);
    }
  }
}
#endif
