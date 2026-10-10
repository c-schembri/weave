#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <atomic>
#include <iostream>

namespace pg = weave::pg;

static void check(bool condition, const char *what)
{
  if (!condition) {
    std::fprintf(stderr, "Native device PG probe: %s\n", what);
    std::_Exit(1);
  }
}

static pg::Options options(
  weave::u16 port,
  std::string address,
  weave::TlsContext tls,
  pg::OAuthProvider provider,
  std::string issuer)
{
  pg::Options value;
  value.host = "localhost";
  value.port = port;
  value.user = "weave";
  value.database = "postgres";
  value.tls = tls;
  value.authentication.methods = {pg::Authentication::oauth};
  value.oauth.emplace();
  value.oauth->issuer = std::move(issuer) + "/.well-known/openid-configuration";
  value.oauth->client_id = "client:/ +&=";
  auto configured = pg::Options::parse("oauth_client_secret='s:e c+/&='");
  check(bool(configured), "Parsed real-server client secret");
  value.oauth->client_secret = configured->oauth->client_secret;
  value.oauth->provider = provider;
  auto parsed = weave::IpAddress::parse(address);
  check(bool(parsed), "PG numeric address");
  value.hosts.push_back({.name = "localhost", .port = port, .address = *parsed});
  return value;
}

static void rows(const pg::Results &result, int expected)
{
  check(
    result.size() == 1 && result.front().rows.size() == 1 &&
      result.front().rows.front().front().integer<int>() == expected,
    "SQL result");
}

static weave::Task<void> session(pg::Options settings)
{
  auto connection = co_await pg::connect(settings);
  check(connection.authentication_method() == pg::Authentication::oauth, "OAuth authentication selected");
  rows(co_await connection.query("SELECT 42"), 42);
  co_await connection.reset(std::move(settings));
  rows(co_await connection.query("SELECT 43"), 43);
  co_await connection.finish();
}

int main()
{
  fixture::Certificates certificates;
  std::cout << certificates.ca << '\n' << certificates.leaf << '\n' << certificates.private_key << std::endl;
  std::string issuer, mode, port_text, address;
  std::getline(std::cin, issuer);
  std::getline(std::cin, mode);
  std::getline(std::cin, port_text);
  std::getline(std::cin, address);
  auto port = weave::parse_port(port_text);
  check(bool(port), "PG port");
  auto tls = weave::TlsContext::client({.ca_file = certificates.ca});
  auto https = weave::TlsContext::client({.ca_file = certificates.ca, .alpn = {"http/1.1"}});
  check(bool(tls) && bool(https), "Independent PostgreSQL and HTTPS TLS contexts");
  std::atomic<unsigned> prompts{0};
  auto handler = [&prompts](pg::OAuthDevicePrompt prompt) noexcept -> weave::Task<void> {
    ++prompts;
    check(prompt.user_code() == "TEST-1234", "Prompt code");
    co_return;
  };
  std::atomic<unsigned> lookups{0};
  auto provider = pg::OAuthProvider::device(
    std::move(handler),
    {.tls = *https},
    [&lookups](const pg::OAuthRequest &request) noexcept -> weave::Result<std::optional<pg::OAuthToken>> {
      ++lookups;
      check(!request.scope_explicit && request.scope.empty(), "Cache miss preserves unspecified scope");
      return std::optional<pg::OAuthToken>{};
    });
  check(bool(provider), "Provider");
  auto ctx = weave::Context::create();
  check(bool(ctx), "Context");
  auto result = ctx->run(session(options(*port, address, *tls, *provider, issuer)));
  if (!result)
    return weave::report_error(result.error());
  std::cout << "Context session/reset verified\n" << std::flush;
  auto blocking = pg::BlockingConnection::connect(options(*port, address, *tls, *provider, issuer));
  if (!blocking)
    return weave::report_error(blocking.error());
  auto first = blocking->query("SELECT 42");
  check(bool(first), "Blocking query");
  rows(*first, 42);
  auto reset = blocking->reset(options(*port, address, *tls, *provider, issuer));
  check(bool(reset), "Blocking reset");
  auto second = blocking->query("SELECT 43");
  check(bool(second), "Blocking query after reset");
  rows(*second, 43);
  check(bool(blocking->finish()), "Blocking finish");
  std::cout << "Blocking session/reset verified\n" << std::flush;

  unsigned expected_prompts = 4;
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(bool(runtime), "Runtime");
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(session(options(*port, address, *tls, *provider, issuer)));
      check(bool(job), "Runtime spawn");
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
    runtime->join();
  }
  expected_prompts += 128;
#endif
  check(prompts == expected_prompts, "Native authorizations through connect/reset");
  check(lookups == expected_prompts, "Explicit discovery cache miss before each native authorization");
  std::cout << "Native device PostgreSQL verified: Context/blocking/connect/reset prompts=" << prompts << std::endl;
}
