#include <weave/postgres.hpp>
#include <weave/io.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <openssl/crypto.h>
#include <iostream>
#include <atomic>
#include <syncstream>

namespace pg = weave::pg;
using namespace std::chrono_literals;

static void check(bool condition, const char *message)
{
  if (!condition) {
    WEAVE_LOG_ERROR("Keycloak: %s", message);
    std::_Exit(1);
  }
}

static void rows(const pg::ResultSet &result)
{
  check(result.rows.size() == 1 && result.rows[0].size() == 3, "SQL shape");
  check(result.rows[0][0].bytes() == "t" && result.rows[0][1].bytes() == "weave", "Verified TLS and real token role");
  check(result.rows[0][2].integer<int>() == 42, "SQL value");
}

static constexpr auto sql = "SELECT ssl, current_user, 42 FROM pg_stat_ssl WHERE pid=pg_backend_pid()";

static weave::Task<void> session(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  check(connection.authentication_method() == pg::Authentication::oauth, "Real OAuth authentication selected");
  rows(co_await connection.execute(sql));
  co_await connection.reset(options);
  check(connection.authentication_method() == pg::Authentication::oauth, "Reset authentication");
  rows(co_await connection.execute(sql));
  co_await connection.finish();
}

static unsigned exercise(pg::Options options)
{
  auto ctx = weave::Context::create();
  check(bool(ctx), "Context");
  auto result = ctx->run(weave::timeout(75s, session(options)));
  if (!result)
    std::_Exit(weave::report_error(result.error()));

  auto blocking = pg::BlockingConnection::connect(options);
  check(bool(blocking) && blocking->authentication_method() == pg::Authentication::oauth, "Blocking authentication");
  auto query = blocking->execute(sql);
  check(bool(query), "Blocking query");
  rows(*query);
  check(bool(blocking->reset(options)), "Blocking reset");
  query = blocking->execute(sql);
  check(bool(query), "Blocking reset query");
  rows(*query);
  check(bool(blocking->finish()), "Blocking finish");

  unsigned authorizations = 4;
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(bool(runtime), "Runtime");
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 8; ++index) {
      auto job = runtime->spawn(weave::timeout(75s, session(options)));
      check(bool(job), "Runtime admission");
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        std::_Exit(weave::report_error(result.error()));
    }
  }
  authorizations += 32;
#endif
  return authorizations;
}

static pg::OAuthProvider cached(std::shared_ptr<const pg::OAuthToken> token, std::atomic<unsigned> &lookups)
{
  auto provider = pg::OAuthProvider::create(
    [](pg::OAuthRequest) noexcept -> weave::Task<pg::OAuthToken> {
      co_await weave::fail(std::errc::state_not_recoverable);
      std::abort();
    },
    [token, &lookups](const pg::OAuthRequest &) noexcept -> weave::Result<std::optional<pg::OAuthToken>> {
      ++lookups;
      auto copy = pg::OAuthToken::parse(token->value());
      if (!copy)
        return std::unexpected(copy.error());
      return std::optional{std::move(*copy)};
    });
  check(bool(provider), "Owning real-token cache");
  return std::move(*provider);
}

static weave::Task<void> rejected(pg::Options options)
{
  auto connection = co_await weave::as_result(pg::connect(std::move(options)));
  check(!connection && pg::sqlstate(connection.error()) == "28000", "Real token rejection");
}

int main()
{
  fixture::Certificates certificates;
  std::cout << certificates.ca << '\n' << certificates.leaf << '\n' << certificates.private_key << std::endl;
  std::string issuer, port_text, client_secret;
  std::getline(std::cin, issuer);
  std::getline(std::cin, port_text);
  std::getline(std::cin, client_secret);
  auto port = weave::parse_port(port_text);
  auto secret = pg::OAuthClientSecret::parse(client_secret);
  OPENSSL_cleanse(client_secret.data(), client_secret.size());
  client_secret.clear();
  check(port && secret, "Owned settings");
  auto tls = weave::TlsContext::client({.ca_file = certificates.ca});
  auto https = weave::TlsContext::client({.ca_file = certificates.ca, .alpn = {"http/1.1"}});
  check(tls && https, "Independent PostgreSQL and HTTPS trust");
  std::atomic<unsigned> prompts{0};
  std::atomic<unsigned> action{0};
  auto native = [&](pg::OAuthClientAuth method) {
    auto provider = pg::OAuthProvider::device(
      [&prompts, &action](pg::OAuthDevicePrompt prompt) noexcept -> weave::Task<void> {
        ++prompts;
        check(!prompt.user_code().empty() && !prompt.verification_uri_complete().empty(), "Real user-facing prompt");
        const auto command = action == 1 ? "DENY " : action == 2 ? "WAIT " : "APPROVE ";
        std::osyncstream(std::cout) << command << prompt.verification_uri_complete() << std::endl;
        co_return;
      },
      {.tls = *https, .client_auth = method});
    check(bool(provider), "Native device provider");
    return std::move(*provider);
  };
  pg::Options options;
  options.host = "localhost";
  options.port = *port;
  options.hosts = {{.name = "localhost", .port = *port, .address = *weave::IpAddress::parse("127.0.0.1")}};
  options.user = "weave";
  options.database = "postgres";
  options.tls = *tls;
  options.authentication.methods = {pg::Authentication::oauth};
  options.oauth.emplace();
  options.oauth->issuer = issuer + "/.well-known/openid-configuration";
  options.oauth->client_id = "weave-pg";
  options.oauth->scope = "openid profile pg-audience";
  options.oauth->client_secret = *secret;
  options.oauth->acquisition_timeout = 60s;

  unsigned expected = 0;
  const std::array methods{pg::OAuthClientAuth::client_secret_basic, pg::OAuthClientAuth::client_secret_post};
  for (auto method : methods) {
    options.oauth->provider = native(method);
    expected += exercise(options);
    check(prompts == expected, "One actual device authorization per uncached connect/reset");
  }

  auto ctx = weave::Context::create();
  check(bool(ctx), "Context");
  auto provider = native(pg::OAuthClientAuth::client_secret_basic);
  pg::OAuthRequest request{
    .issuer = issuer,
    .client_id = "weave-pg",
    .scope = *options.oauth->scope,
    .openid_configuration = options.oauth->issuer,
    .host = "localhost",
    .port = *port,
    .user = "weave",
    .database = "postgres",
    .client_secret = *secret,
    .scope_explicit = true};
  auto acquired = ctx->run(provider.request(request));
  check(bool(acquired), "Real cache credential acquired");
  auto token = std::make_shared<pg::OAuthToken>(std::move(*acquired));
  std::atomic<unsigned> lookups{0};
  options.oauth->provider = cached(token, lookups);
  check(bool(ctx->run(session(options))), "Real cached-token connect/reset");
  check(lookups == 2, "Cache served both sessions without device acquisition");

  std::string altered{token->value()};
  auto signature = altered.rfind('.') + 8;
  check(signature < altered.size(), "Signed JWT shape");
  altered[signature] = altered[signature] == 'a' ? 'b' : 'a';
  auto corrupt = pg::OAuthToken::parse(altered);
  OPENSSL_cleanse(altered.data(), altered.size());
  altered.clear();
  check(bool(corrupt), "Syntactically valid corrupted JWT");
  options.oauth->provider = cached(std::make_shared<pg::OAuthToken>(std::move(*corrupt)), lookups);
  check(bool(ctx->run(rejected(options))), "Signature rejection");

  options.oauth->provider = cached(token, lookups);
  auto other_role = options;
  other_role.user = "other";
  check(bool(ctx->run(rejected(other_role))), "Role rejection");

  request.scope = "openid profile";
  auto no_audience = ctx->run(provider.request(request));
  check(bool(no_audience), "Real token without PostgreSQL audience");
  options.oauth->provider = cached(std::make_shared<pg::OAuthToken>(std::move(*no_audience)), lookups);
  check(bool(ctx->run(rejected(options))), "Audience rejection");

  request.scope = "openid pg-audience";
  auto no_profile = ctx->run(provider.request(request));
  check(bool(no_profile), "Real token without required profile scope");
  options.oauth->provider = cached(std::make_shared<pg::OAuthToken>(std::move(*no_profile)), lookups);
  check(bool(ctx->run(rejected(options))), "Scope rejection");

  check(bool(ctx->run(weave::sleep_for(11s))), "Expiry wait");
  options.oauth->provider = cached(token, lookups);
  check(bool(ctx->run(rejected(options))), "Expired real token rejection");

  options.oauth->provider = provider;
  auto wrong_secret = options;
  wrong_secret.oauth->client_secret = *pg::OAuthClientSecret::parse("wrong-secret");
  auto denied_client = ctx->run(pg::connect(wrong_secret));
  check(!denied_client && denied_client.error() == pg::Error::authentication, "Wrong client credential is terminal");
  check(prompts == expected + 3, "Wrong secret exposes no device prompt");

  auto wrong_trust = weave::TlsContext::client({.ca_file = certificates.untrusted, .alpn = {"http/1.1"}});
  check(bool(wrong_trust), "Independent untrusted HTTPS policy");
  auto untrusted = pg::OAuthProvider::device(
    [](pg::OAuthDevicePrompt) noexcept -> weave::Task<void> {
      check(false, "Untrusted IdP exposed a device prompt");
      co_return;
    },
    {.tls = *wrong_trust});
  check(bool(untrusted), "Untrusted device provider");
  auto wrong_issuer_trust = options;
  wrong_issuer_trust.oauth->provider = *untrusted;
  auto unverified = ctx->run(pg::connect(wrong_issuer_trust));
  check(!unverified && unverified.error() == weave::TlsError::certificate_verification, "HTTPS trust is enforced");

  action = 1;
  auto denied = ctx->run(pg::connect(options));
  check(!denied && denied.error() == std::errc::permission_denied, "User denial is terminal");
  action = 2;
  auto cancelled = ctx->run(weave::timeout(1500ms, pg::connect(options)));
  check(!cancelled && cancelled.error() == std::errc::timed_out, "Pending device acquisition drains on timeout");
  action = 0;
  check(bool(ctx->run(session(options))), "Reuse after denial/timeout");
  check(prompts == expected + 7 && lookups == 7, "Expected real authorization/cache count");
  std::cout << "Real Keycloak controls passed: native=" << expected + 7 << " cache=" << lookups << std::endl;
}
