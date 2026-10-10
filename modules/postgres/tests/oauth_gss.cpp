#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include "tls_certificates.hpp"
#include "gss_acceptor.hpp"
#include "wire.hpp"
#include <atomic>
#include <iostream>
#include <cstring>

namespace pg = weave::pg;
namespace wire = weave::pg::detail;
using namespace std::chrono_literals;

static void check(bool condition, const char *message)
{
  if (!condition) {
    WEAVE_LOG_ERROR("GSS/OAuth: %s", message);
    std::_Exit(1);
  }
}

enum class Phase {
  hit,
  miss,
  rejected,
  stalled
};

struct Counts {
  std::atomic<Phase> phase{Phase::hit};
  std::atomic<unsigned> lookups{0};
  std::atomic<unsigned> acquisitions{0};
  std::atomic<unsigned> prompts{0};
  std::atomic<bool> entered{false};
};

static void protected_rows(const pg::ResultSet &result)
{
  check(result.rows.size() == 1 && result.rows[0].size() == 4, "Protected SQL shape");
  check(result.rows[0][0].bytes() == "t", "Backend confirms GSS encryption");
  check(result.rows[0][1].bytes() == "f", "Backend confirms OAuth did not become GSS authentication");
  check(result.rows[0][2].bytes() == "f", "Backend confirms TLS is absent");
  check(result.rows[0][3].integer<int>() == 42, "SQL value");
}

static constexpr auto
  protected_sql = "SELECT encrypted, gss_authenticated, (SELECT ssl FROM pg_stat_ssl WHERE pid=pg_backend_pid()), 42 "
                  "FROM pg_stat_gssapi WHERE pid=pg_backend_pid()";

static pg::Options configuration(weave::u16 port, std::string issuer, pg::GssContext gss, pg::OAuthProvider oauth)
{
  auto options = pg::Options::parse("oauth_client_secret='s:e c+/&='");
  check(bool(options), "Credential configuration");
  options->host = "localhost";
  options->port = port;
  options->user = "weave";
  options->database = "postgres";
  options->plaintext = true;
  options->gss = gss;
  options->gss_encryption = pg::GssEncryption::require;
  options->authentication.methods = {pg::Authentication::oauth};
  options->hosts = {{.name = "localhost", .port = port, .address = *weave::IpAddress::parse("127.0.0.1")}};
  options->oauth->issuer = std::move(issuer) + "/.well-known/openid-configuration";
  options->oauth->scope = "read write";
  options->oauth->client_id = "client:/ +&=";
  options->oauth->provider = oauth;
  return std::move(*options);
}

static pg::OAuthProvider provider(std::shared_ptr<Counts> counts, weave::TlsContext https, bool native)
{
  pg::OAuthProvider::CacheLookup cached =
    [counts](const pg::OAuthRequest &request) noexcept -> weave::Result<std::optional<pg::OAuthToken>> {
    ++counts->lookups;
    if (request.user != "weave" || request.database != "postgres" || request.scope != "read write" ||
      !request.scope_explicit || !request.client_secret || request.client_secret->value() != "s:e c+/&=")
      return std::unexpected(std::make_error_code(std::errc::bad_message));
    if (counts->phase == Phase::miss || counts->phase == Phase::stalled)
      return std::optional<pg::OAuthToken>{};
    auto token = pg::OAuthToken::parse(counts->phase == Phase::rejected ? "rejected" : "abc");
    return std::optional{std::move(*token)};
  };
  if (native) {
    auto result = pg::OAuthProvider::device(
      [counts](pg::OAuthDevicePrompt prompt) noexcept -> weave::Task<void> {
        ++counts->prompts;
        check(prompt.user_code() == "TEST-1234", "Native prompt");
        co_return;
      },
      {.tls = https},
      std::move(cached));
    check(bool(result), "Native device provider");
    return std::move(*result);
  }
  auto result = pg::OAuthProvider::create(
    [counts](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
      ++counts->acquisitions;
      counts->entered = true;
      check(request.scope == "read write" && request.user == "weave", "Discovered request");
      co_await weave::sleep_for(counts->phase == Phase::stalled ? 1h : 5ms);
      auto token = pg::OAuthToken::parse("abc");
      co_return std::move(*token);
    },
    std::move(cached));
  check(bool(result), "Custom provider");
  return std::move(*result);
}

static weave::Task<void> session(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  check(
    connection.gss_encrypted() && connection.authentication_method() == pg::Authentication::oauth,
    "GSS protects OAuth rather than replacing authentication");
  protected_rows(co_await connection.execute(protected_sql));
  auto large = co_await connection.execute("SELECT repeat('x',200000)");
  check(large.rows[0][0].bytes().size() == 200000, "Protected record fragmentation");
  co_await connection.reset(options);
  check(connection.gss_encrypted(), "Reset stays protected");
  protected_rows(co_await connection.execute(protected_sql));
  co_await connection.finish();
}

static unsigned exercise(pg::Options options)
{
  auto ctx = weave::Context::create();
  check(bool(ctx), "Context");
  auto result = ctx->run(weave::timeout(20s, session(options)));
  if (!result) {
    std::_Exit(weave::report_error(result.error()));
  }
  auto blocking = pg::BlockingConnection::connect(options);
  check(bool(blocking) && blocking->gss_encrypted(), "Blocking GSS/OAuth connect");
  auto rows = blocking->execute(protected_sql);
  check(bool(rows), "Blocking protected query");
  protected_rows(*rows);
  check(bool(blocking->reset(options)), "Blocking reset");
  rows = blocking->execute(protected_sql);
  check(bool(rows), "Blocking query after reset");
  protected_rows(*rows);
  check(bool(blocking->finish()), "Blocking finish");
  unsigned authorizations = 4;
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(bool(runtime), "Runtime");
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(weave::timeout(20s, session(options)));
      check(bool(job), "Runtime admission");
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result) {
        std::_Exit(weave::report_error(result.error()));
      }
    }
  }
  authorizations += 128;
#endif
  return authorizations;
}

static weave::Task<void> interrupted(pg::Connection &connection)
{
  auto query = co_await weave::as_result(connection.execute("SELECT pg_sleep(5)"));
  check(!query && pg::sqlstate(query.error()) == "57014", "Protected backend cancellation reaches the query");
}

static weave::Task<void> dispatch(pg::Options options, pg::CancelHandle handle, int backend)
{
  auto monitor = co_await pg::connect(std::move(options));
  const auto query = "SELECT wait_event = 'PgSleep' AND state = 'active' FROM pg_stat_activity WHERE pid=" +
    std::to_string(backend);
  for (;;) {
    auto activity = co_await monitor.execute(query);
    check(activity.rows.size() == 1, "Target backend remains live");
    if (activity.rows[0][0].bytes() == "t")
      break;
    co_await weave::sleep_for(1ms);
  }
  co_await handle.request();
  co_await monitor.finish();
}

static weave::Task<void> cancel_session(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto handle = connection.cancel_handle();
  check(bool(handle), "Cancel snapshot");
  auto pid = co_await connection.execute("SELECT pg_backend_pid()");
  auto backend = pid.rows[0][0].integer<int>();
  check(bool(backend), "Target backend identity");
  co_await weave::when_all(interrupted(connection), dispatch(options, *handle, *backend));
  auto old = connection.request_cancel();
  co_await connection.reset(options);
  co_await std::move(old);
  protected_rows(co_await connection.execute(protected_sql));
  auto owned = connection.request_cancel();
  co_await connection.finish();
  co_await std::move(owned);
}

static weave::Task<void> waiting(std::shared_ptr<Counts> counts)
{
  while (!counts->entered.load())
    co_await weave::sleep_for(1ms);
}

static weave::Task<wire::Bytes> packet(weave::TcpStream &client)
{
  std::array<std::byte, 4> header;
  co_await client.read_exactly(header);
  wire::Reader reader{header};
  auto size = reader.integer();
  check(size && size <= 65532, "Bounded native packet");
  wire::Bytes body(size);
  co_await client.read_exactly(body);
  co_return body;
}

static weave::Task<wire::SecretStorage<std::byte>> decrypted(weave::TcpStream &client, pg::test::GssAcceptor &server)
{
  auto record = co_await packet(client);
  gss_buffer_desc input{record.size(), record.data()};
  pg::test::NativeBytes output;
  OM_uint32 minor = 0;
  int confidential = 0;
  auto status = gss_unwrap(&minor, server.context, &input, &output.value, &confidential, nullptr);
  check(status == GSS_S_COMPLETE && confidential && output.value.length <= 65536, "Protected peer record");
  co_return output.copy();
}

static weave::Task<void> encrypted_send(
  weave::TcpStream &client,
  pg::test::GssAcceptor &server,
  const wire::Writer &message)
{
  auto record = server.wrap(message.bytes);
  check(bool(record), "Peer encryption");
  wire::Writer packet;
  packet.integer(static_cast<weave::u32>(record->size()));
  packet.raw(*record);
  co_await client.write_all(packet.bytes);
}

static weave::Task<void> closed(weave::TcpStream &client)
{
  std::array<std::byte, 1> buffer;
  auto read = co_await weave::as_result(client.read(buffer));
  check(
    (read && *read == 0) || (!read && read.error() == std::errc::connection_reset),
    "No bytes after security rejection");
}

struct DowngradeCounts {
  unsigned connections = 0;
  bool discovery_closed = false;
};

static weave::Task<void> downgrade_peer(weave::TcpListener &listener, std::string issuer, DowngradeCounts &counts)
{
  {
    auto client = co_await listener.accept();
    ++counts.connections;
    std::array<std::byte, 8> request;
    co_await client.read_exactly(request);
    wire::Reader reader{request};
    check(reader.integer() == 8 && reader.integer() == 80877104, "Initial GSS negotiation");
    const std::array accepted{std::byte{'G'}};
    co_await client.write_all(accepted);

    pg::test::GssAcceptor server;
    check(server.start(), "Native peer credentials");
    auto token = co_await packet(client);
    auto proof = server.accept(token);
    check(proof && server.complete && !proof->empty(), "Mutual native proof");
    wire::Writer reply;
    reply.integer(static_cast<weave::u32>(proof->size()));
    reply.raw(*proof);
    co_await client.write_all(reply.bytes);

    auto startup = co_await decrypted(client, server);
    wire::Reader startup_reader{startup};
    check(startup_reader.integer() == startup.size() && startup_reader.integer() == 196610, "Encrypted startup");
    wire::Writer authentication;
    authentication.integer(10);
    authentication.string("OAUTHBEARER");
    authentication.integer(0, 1);
    reply.bytes.clear();
    reply.message('R', authentication);
    co_await encrypted_send(client, server, reply);

    auto initial = co_await decrypted(client, server);
    constexpr std::string_view empty_token{
      "p\0\0\0\x1f"
      "OAUTHBEARER\0\0\0\0\x0b"
      "n,,\x01"
      "auth=\x01\x01",
      32};
    check(
      std::ranges::equal(initial, std::as_bytes(std::span{empty_token.data(), empty_token.size()})),
      "Protected literal empty-token discovery");
    authentication.bytes.clear();
    authentication.integer(11);
    authentication.raw(
      std::string{"{\"status\":\"invalid_token\",\"scope\":\"read write\",\"openid-configuration\":\""} + issuer +
      "/.well-known/openid-configuration\"}");
    reply.bytes.clear();
    reply.message('R', authentication);
    co_await encrypted_send(client, server, reply);
    auto dummy = co_await decrypted(client, server);
    const std::array
      expected_dummy{std::byte{'p'}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{5}, std::byte{1}};
    check(std::ranges::equal(dummy, expected_dummy), "Protected discovery acknowledgement");

    wire::Writer diagnostic;
    diagnostic.integer('S', 1);
    diagnostic.string("FATAL");
    diagnostic.integer('C', 1);
    diagnostic.string("28000");
    diagnostic.integer('M', 1);
    diagnostic.string("discovery complete");
    diagnostic.integer(0, 1);
    reply.bytes.clear();
    reply.message('E', diagnostic);
    co_await encrypted_send(client, server, reply);
    co_await closed(client);
    counts.discovery_closed = true;
  }

  auto reconnect = co_await listener.accept();
  ++counts.connections;
  std::array<std::byte, 8> request;
  co_await reconnect.read_exactly(request);
  wire::Reader reader{request};
  check(reader.integer() == 8 && reader.integer() == 80877104, "Reconnect still requests GSS encryption");
  const std::array declined{std::byte{'N'}};
  co_await reconnect.write_all(declined);
  co_await closed(reconnect);
}

static weave::Task<void> downgrade(weave::Context &ctx, pg::Options options)
{
  auto listener = weave::tcp::listen(ctx, "127.0.0.1", 0);
  auto fallback = weave::tcp::listen(ctx, "127.0.0.1", 0);
  check(listener && fallback, "Downgrade listeners");
  options.hosts[0].port = listener->local_port();
  options.hosts.push_back(
    {.name = "localhost", .port = fallback->local_port(), .address = *weave::IpAddress::parse("127.0.0.1")});
  options.gss_encryption = pg::GssEncryption::prefer;
  DowngradeCounts counts;
  unsigned acquisitions = 0;
  auto oauth = pg::OAuthProvider::create(
    [&counts, &acquisitions](pg::OAuthRequest) noexcept -> weave::Task<pg::OAuthToken> {
      while (!counts.discovery_closed)
        co_await weave::sleep_for(1ms);
      ++acquisitions;
      co_return std::move(*pg::OAuthToken::parse("abc"));
    });
  check(bool(oauth), "Downgrade provider");
  options.oauth->provider = *oauth;
  auto issuer = options.oauth->issuer;
  issuer.resize(issuer.size() - std::string_view{"/.well-known/openid-configuration"}.size());
  auto peer = ctx.spawn(downgrade_peer(*listener, std::move(issuer), counts));
  check(bool(peer), "Downgrade peer admission");
  auto connection = co_await weave::as_result(pg::connect(options));
  check(
    !connection && connection.error() == pg::Error::authentication,
    "Protected discovery pins reconnect to required GSS");
  co_await std::move(*peer);
  auto untouched = co_await weave::as_result(weave::timeout(20ms, fallback->accept()));
  check(!untouched && untouched.error() == std::errc::timed_out, "No fallback endpoint after downgrade rejection");
  check(counts.connections == 2 && acquisitions == 1, "No duplicate credential acquisition or reconnect");
}

static void failures(pg::Options options, std::shared_ptr<Counts> counts)
{
  auto ctx = weave::Context::create();
  check(bool(ctx), "Failure Context");
  counts->phase = Phase::rejected;
  auto before = counts->lookups.load();
  auto rejected = ctx->run(pg::connect(options));
  check(!rejected && counts->lookups == before + 1, "Rejected cache token is terminal");
  counts->phase = Phase::hit;
  auto invalid = options;
  invalid.gss_mutual = false;
  auto unprotected = ctx->run(pg::connect(std::move(invalid)));
  check(!unprotected && unprotected.error() == std::errc::invalid_argument, "Non-mutual plaintext policy rejected");
  invalid = options;
  invalid.gss_encryption = pg::GssEncryption::disable;
  auto plaintext = ctx->run(pg::connect(std::move(invalid)));
  check(!plaintext && plaintext.error() == std::errc::invalid_argument, "Plaintext OAuth rejected");
  invalid = options;
  invalid.channel_binding = pg::ChannelBinding::require;
  auto binding = ctx->run(pg::connect(std::move(invalid)));
  check(!binding && binding.error() == std::errc::invalid_argument, "Plaintext policy cannot satisfy channel binding");
  invalid = options;
  invalid.plaintext = false;
  invalid.channel_binding = pg::ChannelBinding::require;
  auto protected_binding = ctx->run(pg::connect(std::move(invalid)));
  check(
    !protected_binding && protected_binding.error() == pg::Error::authentication,
    "GSS transport cannot supply TLS channel binding");
  check(counts->lookups == before + 1, "No credential lookup on security-policy rejection");
  invalid = options;
  invalid.gss_service = "weave-nonexistent-service";
  auto wrong_service = ctx->run(pg::connect(std::move(invalid)));
  check(
    !wrong_service && std::string_view{wrong_service.error().category().name()} == "weave.postgres.gssapi" &&
      wrong_service.error().value() == GSS_S_FAILURE,
    "Wrong Kerberos service preserves native authentication failure");
  check(counts->lookups == before + 1, "No credential lookup before mutual server proof");
  counts->phase = Phase::stalled;
  options.oauth->acquisition_timeout = 50ms;
  auto timeout = ctx->run(pg::connect(options));
  check(!timeout && timeout.error() == std::errc::timed_out, "Acquisition deadline drains after GSS discovery");
  options.oauth->acquisition_timeout = 1h;
  counts->entered = false;
  auto job = ctx->spawn(pg::connect(options));
  check(bool(job), "Provider cancellation admission");
  check(bool(ctx->run(weave::timeout(10s, waiting(counts)))), "Provider begins after discovery drain");
  job->cancel();
  auto joining = [&job]() -> weave::Task<void> {
    auto result = co_await weave::as_result(std::move(*job));
    check(!result && result.error() == std::errc::operation_canceled, "Provider cancellation drains");
  };
  check(bool(ctx->run(joining())), "Provider child joined");
  counts->entered = false;
  auto stopping = weave::Context::create();
  check(bool(stopping), "Shutdown Context");
  auto stopped = stopping->spawn(pg::connect(options));
  check(bool(stopped), "Shutdown child admission");
  check(bool(stopping->run(weave::timeout(10s, waiting(counts)))), "Shutdown provider entered");
  stopping->shutdown();
  auto stopped_result = std::move(*stopped).get();
  check(
    !stopped_result && stopped_result.error() == std::errc::operation_canceled,
    "Context shutdown drains provider graph");
  counts->phase = Phase::hit;
  check(bool(ctx->run(session(options))), "Provider slots reusable after failure and cancellation");
}

int main()
{
  fixture::Certificates certificates;
  std::cout << certificates.ca << '\n' << certificates.leaf << '\n' << certificates.private_key << std::endl;
  std::string port_text, issuer, cache;
  std::getline(std::cin, port_text);
  std::getline(std::cin, issuer);
  std::getline(std::cin, cache);
  auto port = weave::parse_port(port_text);
  check(bool(port), "Port");
  auto https = weave::TlsContext::client({.ca_file = certificates.ca, .alpn = {"http/1.1"}});
  check(bool(https), "Independent HTTPS trust");
  auto gss = pg::GssContext::create({.workers = 2, .capacity = 64, .credential_cache = cache});
  check(bool(gss), "Native GSS context");
  const std::array natives{false, true};
  const std::array phases{Phase::hit, Phase::miss};
  for (bool native : natives) {
    for (auto phase : phases) {
      auto counts = std::make_shared<Counts>();
      counts->phase = phase;
      auto oauth = provider(counts, *https, native);
      auto authorizations = exercise(configuration(*port, issuer, *gss, oauth));
      check(counts->lookups == authorizations, "Exactly one cache lookup per authorization");
      check(counts->prompts == (native && phase == Phase::miss ? authorizations : 0), "Native prompt accounting");
      check(
        counts->acquisitions == (!native && phase == Phase::miss ? authorizations : 0),
        "Custom acquisition accounting");
      std::cout << "native=" << native << " hit=" << (phase == Phase::hit) << " authorizations=" << authorizations
                << '\n'
                << std::flush;
    }
  }
  auto single = pg::GssContext::create({.workers = 1, .capacity = 1, .credential_cache = cache});
  check(bool(single), "Single native admission slot");
  auto counts = std::make_shared<Counts>();
  counts->phase = Phase::miss;
  auto oauth = provider(counts, *https, false);
  auto ctx = weave::Context::create();
  check(bool(ctx), "Single-slot Context");
  check(
    bool(ctx->run(session(configuration(*port, issuer, *single, oauth)))),
    "Discovery releases native slot before acquisition/reconnect");
  failures(configuration(*port, issuer, *single, oauth), counts);
  check(
    bool(ctx->run(weave::timeout(10s, downgrade(*ctx, configuration(*port, issuer, *single, oauth))))),
    "Downgrade controls");
  counts->phase = Phase::hit;
  auto cancelled = ctx->run(weave::timeout(10s, cancel_session(configuration(*port, issuer, *gss, oauth))));
  if (!cancelled)
    return weave::report_error(cancelled.error());
  std::cout << "Combined native GSS/OAuth controls passed" << std::endl;
}
