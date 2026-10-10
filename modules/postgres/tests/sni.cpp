#include <weave/postgres.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include "runtime_fixture.hpp"
#endif
#include <atomic>
#include <source_location>
#include <iostream>
#include <cstdio>
#include <cstdlib>

namespace pg = weave::pg;
static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> callbacks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "SNI PostgreSQL check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

static bool negative(std::string_view mode)
{
  return mode == "hostname" || mode == "untrusted";
}

static void configured(const pg::OptionsInfo &info, bool sni)
{
  check(info.server_name_indication == sni);
}

static weave::Task<void> session(pg::Options options, std::string mode)
{
  pg::ConnectionReport report;
  auto connection = co_await weave::as_result(pg::connect(options, report));
  if (negative(mode)) {
    check(!connection && connection.error() == weave::TlsError::certificate_verification);
    check(report.completed && report.attempts.size() == 1);
    co_return;
  }
  check(connection.has_value());
  auto configuration = connection->configuration();
  check(configuration.has_value());
  configured(*configuration, options.server_name_indication);
  auto old = connection->cancel_handle();
  check(old.has_value());

  options.server_name_indication = !options.server_name_indication;
  co_await connection->reset(options);
  configuration = connection->configuration();
  check(configuration.has_value());
  configured(*configuration, options.server_name_indication);
  auto current = connection->cancel_handle();
  check(current.has_value());
  co_await old->request();
  co_await current->request();
  check(connection->open());
  co_await connection->finish();
}

static void blocking(pg::Options options, const std::string &mode)
{
  pg::ConnectionReport report;
  auto connection = pg::BlockingConnection::connect(options, report);
  if (negative(mode)) {
    check(!connection && connection.error() == weave::TlsError::certificate_verification);
    check(report.completed && report.attempts.size() == 1);
    return;
  }
  check(connection.has_value());
  auto configuration = connection->configuration();
  check(configuration.has_value());
  configured(*configuration, options.server_name_indication);
  auto old = connection->cancel_handle();
  check(old.has_value());

  options.server_name_indication = !options.server_name_indication;
  check(connection->reset(options).has_value());
  configuration = connection->configuration();
  check(configuration.has_value());
  configured(*configuration, options.server_name_indication);
  auto current = connection->cancel_handle();
  check(current.has_value());
  check(old->request_blocking().has_value());
  check(current->request_blocking().has_value());
  check(connection->open());
  check(connection->finish().has_value());
}

static void parsing()
{
  const std::array defaults{"user=weave", "user=weave sslsni=1", "user=weave sslsni=''"};
  for (auto text : defaults) {
    auto options = pg::Options::parse(text);
    check(options.has_value() && options->server_name_indication && options->info().server_name_indication);
  }
  const std::array disabled{"user=weave sslsni=0", "postgresql://localhost/postgres?user=weave&sslsni=0"};
  for (auto text : disabled) {
    auto options = pg::Options::parse(text);
    check(options.has_value() && !options->server_name_indication && !options->info().server_name_indication);
  }
  const std::array invalid{"sslsni=true", "sslsni=false", "sslsni=2", "sslsni=-1", "sslsni=01"};
  for (auto text : invalid) {
    auto options = pg::Options::parse(text);
    check(!options && options.error() == std::errc::invalid_argument);
  }
}

int main(int argc, char **argv)
{
  if (argc == 4 && std::string_view(argv[1]) == "--load") {
    auto options = pg::Options::load(argv[3], {.environment = true, .user_files = false, .system_files = false});
    if (std::string_view(argv[2]) == "invalid") {
      check(!options && options.error() == std::errc::invalid_argument);
    } else {
      check(options.has_value());
      check(options->server_name_indication == (std::string_view(argv[2]) == "1"));
      configured(options->info(), options->server_name_indication);
    }
    std::printf("SNI loader: %u checks\n", checks.load());
    return 0;
  }
  check(argc == 5);
  parsing();
  std::string mode = argv[1];
  bool sni = std::string_view(argv[2]) == "1";
  auto version = std::string_view(argv[3]) == "12" ? weave::TlsVersion::tls12 : weave::TlsVersion::tls13;
  auto negotiation = std::string_view(argv[4]) == "direct" ? pg::TlsNegotiation::direct : pg::TlsNegotiation::postgres;
  static fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string first, second;
  check(static_cast<bool>(std::getline(std::cin, first)) && static_cast<bool>(std::getline(std::cin, second)));
  auto port = weave::parse_port(first);
  auto fallback = weave::parse_port(second);
  auto address = weave::IpAddress::parse("127.0.0.1");
  check(port.has_value() && fallback.has_value() && address.has_value());

  pg::Options options;
  options.host = mode == "hostname" ? "wrong.invalid" : "localhost";
  options.user = "weave";
  options.server_name_indication = sni;
  options.tls_negotiation = negotiation;
  options.min_protocol = options.max_protocol = pg::ProtocolVersion::v30;
  options.connect_timeout = std::chrono::seconds{5};
  options.hosts = {{options.host, *port, *address}, {options.host, *fallback, *address}};
  auto credentials = weave::TlsContext::client(
    {.ca_file = mode == "untrusted" ? files.untrusted : files.ca, .min_version = version, .max_version = version});
  check(credentials.has_value());
  options.tls = *credentials;

  if (mode == "oauth") {
    auto provider = pg::OAuthProvider::create([](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
      ++callbacks;
      check(request.issuer == "https://issuer.example/tenant" && request.host == "localhost");
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
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto layout : support::io_layouts) {
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      check(runtime.has_value());
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned i = 0; i < 8; ++i) {
        auto job = runtime->spawn(session(options, mode));
        check(job.has_value());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs)
        check(std::move(job).get().has_value());
      sessions += 8;
    }
  }
#endif
  check(callbacks == (mode == "oauth" ? sessions * 2 : 0));
  std::printf("SNI PostgreSQL: %u checks, %u sessions, %u callbacks\n", checks.load(), sessions, callbacks.load());
}
