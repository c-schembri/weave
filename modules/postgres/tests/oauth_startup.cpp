#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "oauth.hpp"
#include "credential_allocations.hpp"
#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include "tls_certificates.hpp"
#include <atomic>
#include <fstream>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

static std::span<const std::byte> bytes(std::string_view value)
{
  return std::as_bytes(std::span{value.data(), value.size()});
}

static pg::OAuthOptions policy()
{
  return {.issuer = "https://issuer.example/tenant", .client_id = "client"};
}

TEST_CASE("Discovery accepts only byte-identical issuer-bound well-known URLs and configured scopes win")
{
  const std::array urls{
    "https://issuer.example/tenant/.well-known/openid-configuration",
    "https://issuer.example/.well-known/openid-configuration/tenant",
    "https://issuer.example/.well-known/oauth-authorization-server/tenant",
    "https://issuer.example/tenant/.well-known/oauth-authorization-server"};
  for (auto url : urls) {
    auto json = std::string{"{\"status\":\"invalid_token\",\"scope\":\"read write\",\"openid-configuration\":\""} +
      url + "\"}";
    auto result = wire::oauth_discovery(bytes(json), policy());
    REQUIRE(result);
    CHECK(result->scope == "read write");
    CHECK(result->openid_configuration == url);
    auto options = policy();
    options.scope = "configured";
    result = wire::oauth_discovery(bytes(json), options);
    REQUIRE(result);
    CHECK(result->scope == "configured");
    options.scope = "";
    result = wire::oauth_discovery(bytes(json), options);
    REQUIRE(result);
    CHECK(result->scope.empty());
  }
  auto missing = wire::oauth_discovery(bytes("{\"status\":\"invalid_token\"}"), policy());
  REQUIRE(missing);
  CHECK(missing->scope.empty());
  CHECK(missing->openid_configuration == urls.front());

  const std::array hostile{
    "http://issuer.example/tenant/.well-known/openid-configuration",
    "https://ISSUER.example/tenant/.well-known/openid-configuration",
    "https://issuer.example:443/tenant/.well-known/openid-configuration",
    "https://issuer.example@attacker/tenant/.well-known/openid-configuration",
    "https://issuer.example.attacker/tenant/.well-known/openid-configuration",
    "https://issuer.example/tenant/.well-known/openid-configuration?x",
    "https://issuer.example/tenant/.well-known/openid-configuration#x",
    "https://issuer.example/tenant/.well-known/openid-configuration/child",
    "https://issuer.example/.well-known/openid-configuration/other",
    "https://issuer.example/other/.well-known/openid-configuration",
    "https://issuer.example/tenant/.well-known/openid-configuration-bad"};
  for (auto url : hostile) {
    auto json = std::string{"{\"status\":\"invalid_token\",\"openid-configuration\":\""} + url + "\"}";
    auto result = wire::oauth_discovery(bytes(json), policy());
    CHECK((!result && result.error() == pg::Error::authentication));
  }
}

TEST_CASE("JSON is strict, bounded and rejects ambiguous decoded fields")
{
  const std::array malformed{
    "",
    "{}",
    "[]",
    "null",
    "{\"status\":false}",
    "{\"status\":\"invalid_token\",}",
    "{\"status\":\"invalid_token\"} trailing",
    "{\"status\":\"invalid_token\",\"scope\":null}",
    "{\"status\":\"invalid_token\",\"openid-configuration\":1}",
    "{\"status\":\"invalid_token\",\"status\":\"invalid_token\"}",
    "{\"status\":\"invalid_token\",\"sta\\u0074us\":\"invalid_token\"}",
    "{\"status\":\"invalid_token\",\"scope\":\"a\\u0000b\"}",
    "{\"status\":\"invalid_token\",\"scope\":\" read\"}",
    "{\"status\":\"invalid_token\",\"scope\":\"read  write\"}",
    R"({"status":"invalid_token","scope":"read \"})",
    "{\"status\":\"invalid_token\",\"x\":{\"a\":0,\"a\":1}}",
    "{\"status\":\"invalid_token\",\"x\":NaN}",
    "{\"status\":\"invalid_token\",\"x\":\"\xff\"}",
    "{\"status\":\"invalid_token\",\"x\":\"\\ud800\"}"};
  for (auto json : malformed) {
    auto result = wire::oauth_discovery(bytes(json), policy());
    CHECK((!result && result.error() == pg::Error::protocol));
  }
  auto bad_status = wire::oauth_discovery(bytes("{\"status\":\"invalid_scope\"}"), policy());
  CHECK((!bad_status && bad_status.error() == pg::Error::authentication));
  auto large = wire::oauth_discovery(bytes(std::string(65537, ' ')), policy());
  CHECK((!large && large.error() == pg::Error::resource_limit));
  const std::array depths{7u, 8u};
  for (auto depth : depths) {
    auto json = std::string{"{\"status\":\"invalid_token\",\"x\":"} + std::string(depth, '[') + "0" +
      std::string(depth, ']') + "}";
    auto result = wire::oauth_discovery(bytes(json), policy());
    CHECK(bool(result) == (depth == 7));
    if (!result)
      CHECK(result.error() == pg::Error::resource_limit);
  }
}

TEST_CASE("Issuer and provider setup are bounded before any external work")
{
  const std::array valid{
    "https://issuer.example",
    "https://issuer.example/",
    "https://issuer.example:8443/tenant",
    "https://[::1]:8443/tenant",
    "https://127.0.0.1/tenant",
    "https://issuer.example/a%20b"};
  for (auto issuer : valid) {
    auto options = policy();
    options.issuer = issuer;
    CHECK(wire::valid_oauth_options(options));
  }
  const std::array invalid{
    "",
    "http://issuer.example",
    "https://",
    "https://user@host",
    "https://host:",
    "https://host:0",
    "https://host:65536",
    "https://host:1:2",
    "https://[127.0.0.1]",
    "https://[::1",
    "https://[::1]bad",
    "https://host/x?y",
    "https://host/x#y",
    "https://host/a b",
    "https://host/a\\b",
    "https://host/%z1"};
  for (auto issuer : invalid) {
    auto options = policy();
    options.issuer = issuer;
    CHECK_FALSE(wire::valid_oauth_options(options));
  }
}

TEST_CASE("JSON parsing pools cleanse even unknown decoded text on every outcome")
{
  constexpr std::string_view secret = "sensitive-unknown-field-0123456789-sensitive-unknown-field";
  auto json = std::string{"{\"status\":\"invalid_token\",\"unknown\":\""} + std::string{secret} + "\"}";
  fixture::CredentialPattern witness{secret};
  CHECK(wire::oauth_discovery(bytes(json), policy()));
  json.push_back('!');
  CHECK_FALSE(wire::oauth_discovery(bytes(json), policy()));
  CHECK(witness.dirty_releases() == 0);
}

static weave::Task<std::pair<char, wire::Bytes>> receive(auto &stream)
{
  std::array<std::byte, 5> header;
  co_await stream.read_exactly(header);
  wire::Reader reader{header};
  auto kind = static_cast<char>(reader.integer(1));
  auto size = reader.integer();
  if (size < 4 || size > 65536)
    co_await weave::fail(std::errc::bad_message);
  wire::Bytes body(size - 4);
  co_await stream.read_exactly(body);
  co_return std::pair{kind, std::move(body)};
}

static weave::Task<void> authenticate(auto &stream)
{
  std::array<std::byte, 4> length;
  co_await stream.read_exactly(length);
  wire::Reader reader{length};
  auto size = reader.integer();
  if (size < 8 || size > 65536)
    co_await weave::fail(std::errc::bad_message);
  wire::Bytes startup(size - 4);
  co_await stream.read_exactly(startup);
  wire::Writer body;
  body.integer(10);
  body.raw(std::string_view{"OAUTHBEARER\0\0", 13});
  wire::Writer packet;
  packet.message('R', body);
  co_await stream.write_all(packet.bytes);
}

struct Counts {
  unsigned callbacks = 0;
  unsigned connections = 0;
  bool discovery_closed = false;
  std::atomic_bool acquiring = false;
};

enum class Mode {
  normal,
  issuer_mismatch,
  discovery_success,
  duplicate_challenge,
  discovery_stall,
  switched_method,
  unsolicited_success,
  sasl_final,
  reject_then_success,
  provider_failure,
  provider_timeout,
  provider_cancel,
  binding_required
};

static weave::Task<void> expect_closed(auto &stream)
{
  std::array<std::byte, 1> end;
  auto result = co_await weave::as_result(stream.read(end));
  if (result && *result)
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> send_auth(auto &stream, unsigned method, std::string_view body = {})
{
  wire::Writer data;
  data.integer(method);
  data.raw(body);
  wire::Writer packet;
  packet.message('R', data);
  co_await stream.write_all(packet.bytes);
}

static weave::Task<void> peer(weave::TcpListener &listener, weave::TlsContext &tls, Counts &counts, Mode mode)
{
  for (unsigned index = 0; index < 2; ++index) {
    auto raw = co_await listener.accept();
    ++counts.connections;
    std::array<std::byte, 8> ssl;
    co_await raw.read_exactly(ssl);
    wire::Reader reader{ssl};
    if (reader.integer() != 8 || reader.integer() != 80877103)
      co_await weave::fail(std::errc::bad_message);
    constexpr std::array selected{std::byte{'S'}};
    co_await raw.write_all(selected);
    auto stream = co_await weave::tls::server(std::move(raw), tls);
    if (index && (mode == Mode::switched_method || mode == Mode::unsolicited_success)) {
      std::array<std::byte, 4> length;
      co_await stream.read_exactly(length);
      wire::Reader size{length};
      wire::Bytes startup(size.integer() - 4);
      co_await stream.read_exactly(startup);
      co_await send_auth(stream, mode == Mode::switched_method ? 3 : 0);
      co_await expect_closed(stream);
      co_return;
    }
    co_await authenticate(stream);
    if (mode == Mode::binding_required) {
      co_await expect_closed(stream);
      co_return;
    }
    auto initial = co_await receive(stream);
    // Literal length and GS2 bytes, independent from Weave's OAuth response builder.
    constexpr std::string_view discovery{
      "OAUTHBEARER\0\0\0\0\x0b"
      "n,,\x01"
      "auth=\x01\x01",
      27};
    constexpr std::string_view token{
      "OAUTHBEARER\0\0\0\0\x15"
      "n,,\x01"
      "auth=Bearer abc\x01\x01",
      37};
    if (initial.first != 'p' || !std::ranges::equal(initial.second, bytes(index ? token : discovery)))
      co_await weave::fail(std::errc::bad_message);
    if (!index) {
      if (mode == Mode::discovery_stall) {
        co_await expect_closed(stream);
        co_return;
      }
      if (mode == Mode::discovery_success) {
        co_await send_auth(stream, 0);
        co_await expect_closed(stream);
        co_return;
      }
      wire::Writer challenge;
      challenge.integer(11);
      auto issuer = mode == Mode::issuer_mismatch ? "attacker.example" : "issuer.example";
      challenge.raw(
        std::string{"{\"status\":\"invalid_token\",\"scope\":\"read write\","
                    "\"openid-configuration\":\"https://"} +
        issuer + "/tenant/.well-known/openid-configuration\"}");
      wire::Writer packet;
      packet.message('R', challenge);
      co_await stream.write_all(packet.bytes);
      if (mode == Mode::issuer_mismatch) {
        co_await expect_closed(stream);
        co_return;
      }
      auto dummy = co_await receive(stream);
      if (dummy.first != 'p' || dummy.second != wire::Bytes{std::byte{1}})
        co_await weave::fail(std::errc::bad_message);
      if (mode == Mode::duplicate_challenge) {
        co_await stream.write_all(packet.bytes);
        co_await expect_closed(stream);
        co_return;
      }
      wire::Writer diagnostic;
      diagnostic.integer('S', 1);
      diagnostic.string("FATAL");
      diagnostic.integer('C', 1);
      diagnostic.string("28000");
      diagnostic.integer('M', 1);
      diagnostic.string("discovery done");
      diagnostic.integer(0, 1);
      packet.bytes.clear();
      packet.message('E', diagnostic);
      co_await stream.write_all(packet.bytes);
      std::array<std::byte, 1> end;
      auto closed = co_await weave::as_result(stream.read(end));
      if (closed && *closed != 0)
        co_await weave::fail(std::errc::bad_message);
      counts.discovery_closed = true;
      if (mode == Mode::provider_failure || mode == Mode::provider_timeout || mode == Mode::provider_cancel)
        co_return;
    } else {
      if (mode == Mode::sasl_final) {
        co_await send_auth(stream, 12);
        co_await expect_closed(stream);
        co_return;
      }
      if (mode == Mode::reject_then_success) {
        co_await send_auth(stream, 11, R"({"status":"invalid_token"})");
        auto dummy = co_await receive(stream);
        if (dummy.first != 'p' || dummy.second != wire::Bytes{std::byte{1}})
          co_await weave::fail(std::errc::bad_message);
        co_await send_auth(stream, 0);
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
  }
}

static weave::Task<void> scenario(
  weave::TlsContext client_tls,
  weave::TlsContext server_tls,
  Mode mode = Mode::normal,
  std::shared_ptr<Counts> observation = {})
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  auto fallback = co_await weave::tcp::listen("127.0.0.1", 0);
  auto owner = observation ? std::move(observation) : std::make_shared<Counts>();
  auto &counts = *owner;
  auto provider = pg::OAuthProvider::create(
    [owner, mode](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
      auto &counts = *owner;
      ++counts.callbacks;
      counts.acquiring = true;
      // The peer must receive the close while the callback is doing asynchronous work.
      bool pending = mode == Mode::provider_timeout || mode == Mode::provider_cancel;
      co_await weave::sleep_for(pending ? 1h : 750ms);
      if (!counts.discovery_closed || request.issuer != "https://issuer.example/tenant" ||
        request.scope != "read write" || request.client_id != "client" || request.user != "user")
        co_await weave::fail(std::errc::bad_message);
      if (mode == Mode::provider_failure)
        co_await weave::fail(std::errc::permission_denied);
      auto token = pg::OAuthToken::parse("abc");
      if (!token)
        co_await weave::fail(token.error());
      co_return std::move(*token);
    });
  if (!provider)
    co_await weave::fail(provider.error());
  auto options = pg::Options{};
  options.host = "127.0.0.1";
  options.port = listener.local_port();
  options.hosts = {
    {.name = "127.0.0.1", .port = listener.local_port()},
    {.name = "127.0.0.1", .port = fallback.local_port()}};
  options.user = "user";
  options.tls = client_tls;
  options.authentication.methods = {pg::Authentication::oauth};
  if (mode == Mode::binding_required)
    options.channel_binding = pg::ChannelBinding::require;
  options.connect_timeout = 500ms;
  options.oauth = policy();
  options.oauth->provider = *provider;
  if (mode == Mode::provider_timeout)
    options.oauth->acquisition_timeout = 50ms;
  auto client = [&options, mode]() -> weave::Task<void> {
    pg::ConnectionReport report;
    auto result = co_await weave::as_result(pg::connect(std::move(options), report));
    CHECK(report.completed);
    CHECK(report.error == (result ? std::error_code{} : result.error()));
    CHECK_FALSE(report.truncated);
    if (result) {
      CHECK(report.attempts.empty());
    } else {
      REQUIRE(report.attempts.size() == 1);
      auto provider_failure = mode == Mode::provider_failure || mode == Mode::provider_timeout ||
        mode == Mode::provider_cancel;
      auto expected_stage = provider_failure ? pg::ConnectionStage::oauth : pg::ConnectionStage::authentication;
      CAPTURE(static_cast<int>(mode));
      CHECK(report.attempts[0].stage == expected_stage);
      CHECK(report.attempts[0].error == result.error());
      CHECK(report.attempts[0].endpoint.has_value());
      CHECK(report.attempts[0].host == "127.0.0.1");
      CHECK(report.attempts[0].diagnostic.fields.empty());
      CHECK(report.format().has_value());
    }
    if (mode == Mode::provider_cancel && !result)
      co_await weave::fail(result.error());
    if (mode != Mode::normal) {
      std::error_code expected = pg::Error::authentication;
      if (mode == Mode::duplicate_challenge || mode == Mode::sasl_final)
        expected = pg::Error::protocol;
      else if (mode == Mode::discovery_stall || mode == Mode::provider_timeout)
        expected = std::make_error_code(std::errc::timed_out);
      else if (mode == Mode::provider_failure)
        expected = std::make_error_code(std::errc::permission_denied);
      if (result || result.error() != expected)
        co_await weave::fail(std::errc::bad_message);
      co_return;
    }
    if (!result)
      co_await weave::fail(result.error());
    if (result->authentication_method() != pg::Authentication::oauth)
      co_await weave::fail(std::errc::bad_message);
    co_await result->finish();
  };
  co_await weave::when_all(peer(listener, server_tls, counts, mode), client());
  bool early = mode == Mode::issuer_mismatch || mode == Mode::discovery_success || mode == Mode::duplicate_challenge ||
    mode == Mode::discovery_stall || mode == Mode::binding_required;
  bool no_reconnect = early || mode == Mode::provider_failure || mode == Mode::provider_timeout;
  if (counts.callbacks != (early ? 0u : 1u) || counts.connections != (no_reconnect ? 1u : 2u))
    co_await weave::fail(std::errc::bad_message);

  // A second configured host must remain untouched after any post-connect failure.
  auto unused = co_await weave::as_result(weave::timeout(5ms, fallback.accept()));
  if (unused || unused.error() != std::errc::timed_out)
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> wait_acquiring(const Counts &counts)
{
  while (!counts.acquiring.load())
    co_await weave::sleep_for(1ms);
}

TEST_CASE("Cancellation and Context shutdown drain the discovery/provider graph and release owning closures")
{
  fixture::Certificates certificates;
  auto client = weave::TlsContext::client({.ca_file = certificates.ca});
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  REQUIRE(client);
  REQUIRE(server);
  const std::array shutdowns{false, true};
  for (bool shutdown : shutdowns) {
    auto ctx = weave::Context::create();
    REQUIRE(ctx);
    auto observation = std::make_shared<Counts>();
    std::weak_ptr weak = observation;
    auto job = ctx->spawn(scenario(*client, *server, Mode::provider_cancel, observation));
    REQUIRE(job);
    REQUIRE(ctx->run(weave::timeout(2s, wait_acquiring(*observation))));
    if (shutdown) {
      ctx->shutdown();
    } else {
      job->cancel();
      auto join = [&job]() -> weave::Task<void> {
        co_await std::move(*job);
      };
      auto cancelled = ctx->run(join());
      CHECK((!cancelled && cancelled.error() == std::errc::operation_canceled));
    }
    CHECK(observation->callbacks == 1);
    CHECK(observation->connections == 1);
    observation.reset();
    CHECK(weak.expired());
  }
}

TEST_CASE("OAuth parsing owns configuration, preserves empty scope, and needs an explicit provider")
{
  auto options = pg::Options::parse(
    "require_auth=oauth oauth_issuer=https://issuer.example/tenant oauth_client_id=client oauth_scope=''");
  REQUIRE(options);
  REQUIRE(options->oauth);
  CHECK(options->oauth->issuer == "https://issuer.example/tenant");
  CHECK(options->oauth->client_id == "client");
  REQUIRE(options->oauth->scope);
  CHECK(options->oauth->scope->empty());
  CHECK_FALSE(options->oauth->provider);
  options->user = "user";
  options->host = "not a host";
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto unavailable = ctx->run(pg::connect(*options));
  CHECK((!unavailable && unavailable.error() == std::errc::operation_not_supported));
  auto confidential = pg::Options::parse("oauth_client_secret=secret");
  REQUIRE(confidential);
  REQUIRE(confidential->oauth);
  REQUIRE(confidential->oauth->client_secret);
  CHECK(confidential->oauth->client_secret->value() == "secret");

  auto uri = pg::Options::parse(
    "postgres://localhost/"
    "postgres?oauth_issuer=https%3A%2F%2Fissuer.example%2Ftenant&oauth_client_id=client&oauth_scope=read%20write");
  REQUIRE(uri);
  REQUIRE(uri->oauth);
  CHECK(uri->oauth->issuer == "https://issuer.example/tenant");
  CHECK(uri->oauth->scope == "read write");

  fixture::Certificates temporary;
  auto service = std::filesystem::path{temporary.ca}.parent_path() / "oauth.conf";
  {
    std::ofstream file{service};
    file << "[oauth]\noauth_issuer=https://issuer.example/tenant\noauth_client_id=service-client\noauth_scope=read "
            "write\n";
    file.close();
    REQUIRE(file);
  }
  pg::ConfigSources sources{.environment = false, .user_files = false, .service_file = service.string()};
  auto loaded = pg::Options::load("service=oauth oauth_client_id=explicit oauth_scope=''", sources);
  REQUIRE(loaded);
  REQUIRE(loaded->oauth);
  CHECK(loaded->oauth->issuer == "https://issuer.example/tenant");
  CHECK(loaded->oauth->client_id == "explicit");
  CHECK(loaded->oauth->scope == "");

  auto provider = pg::OAuthProvider::create([](pg::OAuthRequest) noexcept -> weave::Task<pg::OAuthToken> {
    co_await weave::fail(std::errc::bad_message);
    std::abort();
  });
  REQUIRE(provider);
  loaded->user = "user";
  loaded->host = "not a host";
  loaded->oauth->provider = *provider;
  loaded->plaintext = true;
  auto plaintext = ctx->run(pg::connect(*loaded));
  CHECK((!plaintext && plaintext.error() == std::errc::invalid_argument));
}

TEST_CASE("TLS discovery socket closes before provider and the pinned reconnect authenticates")
{
  fixture::Certificates certificates;
  auto client = weave::TlsContext::client({.ca_file = certificates.ca});
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  REQUIRE(client);
  REQUIRE(server);
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  auto result = ctx->run(scenario(*client, *server));
  auto error = result ? std::string{} : result.error().message();
  INFO(error);
  CHECK(result);
}

TEST_CASE("Hostile discovery and reconnect sequences do not become success or reacquire credentials")
{
  fixture::Certificates certificates;
  auto client = weave::TlsContext::client({.ca_file = certificates.ca});
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  REQUIRE(client);
  REQUIRE(server);
  auto ctx = weave::Context::create();
  REQUIRE(ctx);
  const std::array modes{
    Mode::issuer_mismatch,
    Mode::discovery_success,
    Mode::duplicate_challenge,
    Mode::discovery_stall,
    Mode::switched_method,
    Mode::unsolicited_success,
    Mode::sasl_final,
    Mode::reject_then_success,
    Mode::provider_failure,
    Mode::provider_timeout,
    Mode::binding_required};
  for (auto mode : modes) {
    auto result = ctx->run(scenario(*client, *server, mode));
    auto error = result ? std::string{} : result.error().message();
    INFO(error);
    INFO(static_cast<unsigned>(mode));
    REQUIRE(result);
  }
}

#include "oauth_cached.hpp"

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
static weave::Task<void> rejected_tls_peer(weave::TcpListener &listener, weave::TlsContext credentials)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 8> request;
  co_await socket.read_exactly(request);
  const std::array selected{std::byte{'S'}};
  co_await socket.write_all(selected);
  auto handshake = co_await weave::as_result(weave::tls::server(std::move(socket), credentials));
}

static weave::Task<void> rejected_tls(weave::TlsContext client, weave::TlsContext server)
{
  auto listener = co_await weave::tcp::listen("127.0.0.1", 0);
  pg::Options options{.host = "mismatch.example", .port = listener.local_port(), .user = "probe", .tls = client};
  options.hosts = {{"mismatch.example", listener.local_port(), weave::IpAddress::loopback_v4()}};
  pg::ConnectionReport report;
  auto connecting = [&]() -> weave::Task<void> {
    auto result = co_await weave::as_result(pg::connect(options, report));
    CHECK_FALSE(result);
    CHECK(report.completed);
    CHECK(report.error == result.error());
    REQUIRE(report.attempts.size() == 1);
    CHECK(report.attempts[0].stage == pg::ConnectionStage::tls);
    CHECK(report.attempts[0].host == "mismatch.example");
    CHECK(report.attempts[0].endpoint.has_value());
    CHECK(report.attempts[0].diagnostic.fields.empty());
    CHECK_FALSE(report.truncated);
    CHECK(report.format().has_value());
  };
  co_await weave::when_all(rejected_tls_peer(listener, server), connecting());
}

TEST_CASE("Reported TLS verification and OAuth failures survive concurrent runtime migration")
{
  fixture::Certificates certificates;
  auto client = weave::TlsContext::client({.ca_file = certificates.ca});
  auto server = weave::TlsContext::server(
    {.certificate_file = certificates.leaf, .private_key_file = certificates.private_key});
  REQUIRE(client);
  REQUIRE(server);
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    REQUIRE(runtime);
    const std::array modes{Mode::normal, Mode::provider_failure, Mode::provider_timeout, Mode::switched_method};
    for (auto mode : modes) {
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned index = 0; index < 16; ++index) {
        auto job = runtime->spawn(scenario(*client, *server, mode));
        REQUIRE(job);
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs)
        CHECK(std::move(job).get());
    }
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn(rejected_tls(*client, *server));
      REQUIRE(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      CHECK(std::move(job).get());
  }
}
#endif
