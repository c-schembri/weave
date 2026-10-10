#include <weave/postgres.hpp>
#include <weave/io.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/log.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#include <libpq-fe.h>
#include <atomic>
#include <iostream>
#include <cstring>

namespace pg = weave::pg;
static std::atomic<unsigned> baseline_lookups{0};

static void token_cleanup(PGconn *, PGoauthBearerRequest *request)
{
  std::free(request->token);
}

static int token_hook(PGauthData type, PGconn *connection, void *data)
{
  if (type != PQAUTHDATA_OAUTH_BEARER_TOKEN)
    return PQdefaultAuthDataHook(type, connection, data);
  auto *request = static_cast<PGoauthBearerRequest *>(data);
  ++baseline_lookups;
  if (!request->openid_configuration || !request->scope ||
    std::string_view{request->openid_configuration} !=
      "https://issuer.example/tenant/.well-known/openid-configuration" ||
    std::string_view{request->scope} != "read write")
    return -1;
  request->token = static_cast<char *>(std::malloc(4));
  if (!request->token)
    return 0;
  std::memcpy(request->token, "abc", 4);
  request->cleanup = token_cleanup;
  return 1;
}

static pg::Options options(weave::u16 port, std::string hostaddr, weave::TlsContext tls, pg::OAuthProvider provider)
{
  pg::Options value;
  value.host = "localhost";
  value.port = port;
  value.user = "weave";
  value.database = "postgres";
  value.tls = tls;
  value.authentication.methods = {pg::Authentication::oauth};
  value.oauth.emplace();
  value.oauth->issuer = "https://issuer.example/tenant/.well-known/openid-configuration";
  value.oauth->client_id = "client";
  value.oauth->scope = "read write";
  auto configured = pg::Options::parse("oauth_client_secret='s:e c+/&='");
  fixture::require(bool(configured));
  value.oauth->client_secret = configured->oauth->client_secret;
  value.oauth->provider = provider;
  auto address = weave::IpAddress::parse(hostaddr);
  fixture::require(bool(address));
  value.hosts.push_back({.name = "localhost", .port = port, .address = *address});
  return value;
}

static weave::Task<void> session(pg::Options settings)
{
  auto connection = co_await pg::connect(settings);
  if (connection.authentication_method() != pg::Authentication::oauth)
    co_await weave::fail(std::errc::bad_message);
  auto rows = co_await connection.query("SELECT 42");
  if (rows.size() != 1 || rows.front().rows.size() != 1 || rows.front().rows.front().front().integer<int>() != 42)
    co_await weave::fail(std::errc::bad_message);
  co_await connection.reset(std::move(settings));
  rows = co_await connection.query("SELECT 43");
  if (rows.size() != 1 || rows.front().rows.size() != 1 || rows.front().rows.front().front().integer<int>() != 43)
    co_await weave::fail(std::errc::bad_message);
  co_await connection.finish();
}

int main()
{
  fixture::Certificates certificates;
  std::cout << certificates.ca << '\n' << certificates.leaf << '\n' << certificates.private_key << '\n' << std::flush;
  std::string port_text, address;
  if (!std::getline(std::cin, port_text) || !std::getline(std::cin, address))
    return 2;
  auto port = weave::parse_port(port_text);
  if (!port)
    return 2;
  auto tls = weave::TlsContext::client({.ca_file = certificates.ca});
  if (!tls)
    return weave::report_error(tls.error());

  std::atomic<unsigned> callbacks{0};
  std::atomic<unsigned> lookups{0};
  std::atomic<bool> miss{false};
  auto provider = pg::OAuthProvider::create(
    [&callbacks](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
      ++callbacks;
      if (request.issuer != "https://issuer.example/tenant" || request.scope != "read write" ||
        request.client_id != "client" || request.user != "weave" || request.database != "postgres" ||
        !request.client_secret || request.client_secret->value() != "s:e c+/&=")
        co_await weave::fail(std::errc::bad_message);
      auto token = pg::OAuthToken::parse("abc");
      if (!token)
        co_await weave::fail(token.error());
      co_return std::move(*token);
    },
    [&](const pg::OAuthRequest &request) noexcept -> weave::Result<std::optional<pg::OAuthToken>> {
      ++lookups;
      if (request.issuer != "https://issuer.example/tenant" || request.scope != "read write" ||
        !request.scope_explicit || request.client_id != "client" || request.user != "weave" ||
        request.database != "postgres" ||
        request.openid_configuration != "https://issuer.example/tenant/.well-known/openid-configuration")
        return std::unexpected(std::make_error_code(std::errc::bad_message));
      if (miss)
        return std::optional<pg::OAuthToken>{};
      auto token = pg::OAuthToken::parse("abc");
      return std::optional{std::move(*token)};
    });
  if (!provider)
    return weave::report_error(provider.error());
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  const std::array phases{false, true};
  for (bool fallback : phases) {
    miss = fallback;
    auto result = ctx->run(session(options(*port, address, *tls, *provider)));
    if (!result)
      return weave::report_error(result.error());
    auto blocking = pg::BlockingConnection::connect(options(*port, address, *tls, *provider));
    if (!blocking)
      return weave::report_error(blocking.error());
    auto rows = blocking->query("SELECT 42");
    if (!rows || rows->front().rows.front().front().integer<int>() != 42)
      return 3;
    if (auto reset = blocking->reset(options(*port, address, *tls, *provider)); !reset)
      return weave::report_error(reset.error());
    rows = blocking->query("SELECT 43");
    if (!rows || rows->front().rows.front().front().integer<int>() != 43)
      return 3;
    if (auto finished = blocking->finish(); !finished)
      return weave::report_error(finished.error());

    unsigned expected_callbacks = 4;
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
      if (!runtime)
        return weave::report_error(runtime.error());
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned index = 0; index < 32; ++index) {
        auto job = runtime->spawn(session(options(*port, address, *tls, *provider)));
        if (!job)
          return weave::report_error(job.error());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs) {
        auto result = std::move(job).get();
        if (!result)
          return weave::report_error(result.error());
      }
      runtime->join();
    }
    expected_callbacks += 128;
#endif
    if (callbacks != (fallback ? expected_callbacks : 0) || lookups != expected_callbacks * (fallback ? 2 : 1))
      return 3;
  }

  PQsetAuthDataHook(token_hook);
  auto number = std::to_string(*port);
  const std::array<const char *, 13> keys{
    "host",
    "hostaddr",
    "port",
    "user",
    "dbname",
    "sslmode",
    "sslrootcert",
    "gssencmode",
    "require_auth",
    "oauth_issuer",
    "oauth_client_id",
    "oauth_scope",
    nullptr};
  const std::array<const char *, 13> values{
    "localhost",
    address.c_str(),
    number.c_str(),
    "weave",
    "postgres",
    "verify-full",
    certificates.ca.c_str(),
    "disable",
    "oauth",
    "https://issuer.example/tenant/.well-known/openid-configuration",
    "client",
    "read write",
    nullptr};
  auto *connection = PQconnectdbParams(keys.data(), values.data(), 0);
  if (PQstatus(connection) != CONNECTION_OK) {
    WEAVE_LOG_ERROR("libpq: %s", PQerrorMessage(connection));
    PQfinish(connection);
    return 4;
  }
  auto *baseline = PQexec(connection, "SELECT 42");
  bool matched = PQresultStatus(baseline) == PGRES_TUPLES_OK && PQntuples(baseline) == 1 &&
    std::string_view{PQgetvalue(baseline, 0, 0)} == "42";
  PQclear(baseline);
  PQreset(connection);
  matched = matched && PQstatus(connection) == CONNECTION_OK;
  PQfinish(connection);
  matched = matched && baseline_lookups == 2;
  std::cout << "OAuth cached real-server callbacks=" << callbacks << " lookups=" << lookups
            << " libpq_cache_lookups=" << baseline_lookups << " libpq_control=" << matched << '\n';
  return matched ? 0 : 4;
}
