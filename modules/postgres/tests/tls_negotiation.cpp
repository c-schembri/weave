#include <weave/postgres.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include "runtime_fixture.hpp"
#endif
#include <atomic>
#include <iostream>
#include <source_location>
#include <cstdio>
#include <cstdlib>

namespace pg = weave::pg;
static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> callbacks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Direct TLS negotiation failed: %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

static bool negative(std::string_view mode)
{
  return mode == "missing" || mode == "unrelated" || mode == "untrusted" || mode == "hostname" ||
    mode == "oauth-missing";
}

static void failure(std::error_code error, std::string_view mode)
{
  if (mode == "untrusted" || mode == "hostname")
    check(error == weave::TlsError::certificate_verification);
  else
    check(error == weave::TlsError::protocol);
}

static weave::Task<void> session(pg::Options options, std::string mode)
{
  pg::ConnectionReport report;
  auto result = co_await weave::as_result(pg::connect(options, report));

  if (negative(mode)) {
    check(!result && report.completed && report.attempts.size() == 1);
    failure(result.error(), mode);
    co_return;
  }

  check(result.has_value() && report.completed && report.attempts.empty());
  auto info = result->info();
  check(info.has_value() && info->tls && info->tls->negotiated_protocol == "postgresql");
  check(info->endpoint && info->endpoint->port == options.hosts.front().port);

  if (mode == "reset") {
    pg::ConnectionReport reset_report;
    auto reset = co_await weave::as_result(result->reset(options, reset_report));
    check(!reset && reset_report.completed && reset_report.attempts.size() == 1 && !result->open());
    failure(reset.error(), mode);
    auto saved = result->configuration();
    check(saved.has_value() && saved->tls_negotiation == pg::TlsNegotiation::direct);
    co_return;
  }

  if (mode == "cancel") {
    auto handle = result->cancel_handle();
    check(handle.has_value());
    auto canceled = co_await weave::as_result(handle->request());
    check(!canceled && result->open());
    failure(canceled.error(), mode);
  } else {
    check(result->authentication_method() == pg::Authentication::oauth);
    co_await result->reset(options);
    info = result->info();
    check(info.has_value() && info->tls && info->tls->negotiated_protocol == "postgresql");
  }

  co_await result->finish();
}

static void blocking(pg::Options options, const std::string &mode)
{
  pg::ConnectionReport report;
  auto result = pg::BlockingConnection::connect(options, report);

  if (negative(mode)) {
    check(!result && report.completed && report.attempts.size() == 1);
    failure(result.error(), mode);
    return;
  }

  check(result.has_value() && report.completed && report.attempts.empty());
  auto info = result->info();
  check(info.has_value() && info->tls && info->tls->negotiated_protocol == "postgresql");
  check(info->endpoint && info->endpoint->port == options.hosts.front().port);

  if (mode == "reset") {
    pg::ConnectionReport reset_report;
    auto reset = result->reset(options, reset_report);
    check(!reset && reset_report.completed && reset_report.attempts.size() == 1 && !result->open());
    failure(reset.error(), mode);
    auto saved = result->configuration();
    check(saved.has_value() && saved->tls_negotiation == pg::TlsNegotiation::direct);
    return;
  }

  if (mode == "cancel") {
    auto handle = result->cancel_handle();
    check(handle.has_value());
    auto canceled = handle->request_blocking();
    check(!canceled && result->open());
    failure(canceled.error(), mode);
  } else {
    check(result->authentication_method() == pg::Authentication::oauth);
    check(result->reset(options).has_value());
    info = result->info();
    check(info.has_value() && info->tls && info->tls->negotiated_protocol == "postgresql");
  }

  check(result->finish().has_value());
}

int main(int argc, char **argv)
{
  check(argc == 2);
  std::string mode = argv[1];
  static fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);

  std::string first, second;
  check(static_cast<bool>(std::getline(std::cin, first)) && static_cast<bool>(std::getline(std::cin, second)));
  auto primary = weave::parse_port(first);
  auto fallback = weave::parse_port(second);
  auto address = weave::IpAddress::parse("127.0.0.1");
  check(primary.has_value() && fallback.has_value() && address.has_value());

  pg::Options options;
  options.host = "localhost";
  options.user = "weave";
  options.tls_negotiation = pg::TlsNegotiation::direct;
  options.connect_timeout = std::chrono::seconds{2};
  options.hosts = {{"localhost", *primary, *address}, {"localhost", *fallback, *address}};

  if (mode == "hostname") {
    options.host = "wrong.invalid";
    for (auto &host : options.hosts)
      host.name = options.host;
  }

  auto tls = weave::TlsContext::client(
    {.ca_file = mode == "untrusted" ? files.untrusted : files.ca, .alpn = {"unrelated"}});
  check(tls.has_value());
  options.tls = *tls;

  const bool oauth = mode == "oauth" || mode == "oauth-missing";
  if (oauth) {
    auto provider = pg::OAuthProvider::create([](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
      ++callbacks;
      check(request.issuer == "https://issuer.example/tenant" && request.scope == "read write");
      check(request.host == "localhost" && request.user == "weave");

      auto token = pg::OAuthToken::parse("abc");
      check(token.has_value());
      co_return std::move(*token);
    });

    check(provider.has_value());
    options.authentication.methods = {pg::Authentication::oauth};
    options.oauth.emplace();
    options.oauth->issuer = "https://issuer.example/tenant";
    options.oauth->client_id = "client";
    options.oauth->provider = *provider;
  }

  auto ctx = weave::Context::create();
  check(ctx.has_value());
  check(ctx->run(session(options, mode)).has_value());
  blocking(options, mode);

  unsigned sessions = 2;
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  if (mode == "oauth" || mode == "missing" || mode == "unrelated" || mode == "untrusted" || mode == "hostname") {
    const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
    for (auto layout : support::io_layouts) {
      for (auto scheduler : schedulers) {
        auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
        check(runtime.has_value());

        std::vector<weave::JoinHandle<void>> jobs;
        for (int i = 0; i < 8; ++i) {
          auto job = runtime->spawn(session(options, mode));
          check(job.has_value());
          jobs.push_back(std::move(*job));
        }

        for (auto &job : jobs)
          check(std::move(job).get().has_value());
        sessions += 8;
      }
    }
  }
#endif

  if (oauth)
    check(callbacks == (mode == "oauth" ? sessions * 2 : 2u));

  std::printf("Direct TLS negotiation %s: %u checks, %u callbacks\n", mode.c_str(), checks.load(), callbacks.load());
}
