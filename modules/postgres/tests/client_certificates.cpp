#include <weave/postgres.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include "runtime_fixture.hpp"
#endif
#include <atomic>
#include <source_location>
#include <iostream>
#include <cstdio>

namespace pg = weave::pg;
static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> providers{0};
static std::atomic<unsigned> oauth_callbacks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Certificate PostgreSQL check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(1);
  }
}

static void configured(const pg::OptionsInfo &info, const pg::Options &options)
{
  check(info.client_certificate == options.client_certificate);
  if (options.tls_options) {
    check(info.tls_options.has_value());
    check(info.tls_options->certificate_file == options.tls_options->certificate_file);
    check(info.tls_options->private_key_file == options.tls_options->private_key_file);
  }
}

static void reset_policy(pg::Options &options)
{
  const bool disabled = options.client_certificate == weave::TlsCertificateMode::disable;
  options.client_certificate = disabled ? weave::TlsCertificateMode::allow : weave::TlsCertificateMode::disable;
  if (options.tls_options && options.tls_options->certificate_file.ends_with("missing.pem")) {
    options.tls_options->certificate_file.clear();
    options.tls_options->private_key_file.clear();
    options.tls_options->private_key_password_provider.reset();
  }
}

static weave::Task<void> session(pg::Options options, bool success)
{
  pg::ConnectionReport report;
  auto connection = co_await weave::as_result(pg::connect(options, report));
  if (!success) {
    check(!connection && connection.error() == weave::TlsError::client_certificate_required);
    check(report.completed && report.attempts.size() == 1);
    co_return;
  }
  check(connection.has_value());
  auto configuration = connection->configuration();
  check(configuration.has_value());
  configured(*configuration, options);
  auto old = connection->cancel_handle();
  check(old.has_value());

  reset_policy(options);
  co_await connection->reset(options);
  configuration = connection->configuration();
  check(configuration.has_value());
  configured(*configuration, options);
  auto current = connection->cancel_handle();
  check(current.has_value());
  co_await old->request();
  co_await current->request();
  check(connection->open());
  co_await connection->finish();
}

static void blocking(pg::Options options, bool success)
{
  pg::ConnectionReport report;
  auto connection = pg::BlockingConnection::connect(options, report);
  if (!success) {
    check(!connection && connection.error() == weave::TlsError::client_certificate_required);
    check(report.completed && report.attempts.size() == 1);
    return;
  }
  check(connection.has_value());
  auto configuration = connection->configuration();
  check(configuration.has_value());
  configured(*configuration, options);
  auto old = connection->cancel_handle();
  check(old.has_value());

  reset_policy(options);
  check(connection->reset(options).has_value());
  configuration = connection->configuration();
  check(configuration.has_value());
  configured(*configuration, options);
  auto current = connection->cancel_handle();
  check(current.has_value());
  check(old->request_blocking().has_value());
  check(current->request_blocking().has_value());
  check(connection->open());
  check(connection->finish().has_value());
}

int main(int argc, char **argv)
{
  check(argc == 6);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  const std::string_view policy = argv[1];
  const std::string_view identity = argv[2];
  const bool request = std::string_view(argv[3]) == "1";
  const bool plaintext = std::string_view(argv[5]) == "plaintext";
  const auto version = std::string_view(argv[4]) == "12" ? weave::TlsVersion::tls12 : weave::TlsVersion::tls13;
  const auto negotiation = std::string_view(argv[5]) == "direct" ? pg::TlsNegotiation::direct
                                                                 : pg::TlsNegotiation::postgres;
  static fixture::Certificates files;
  std::printf(
    "%s\n%s\n%s\n%s\n%s\n",
    files.ca.c_str(),
    files.leaf.c_str(),
    files.private_key.c_str(),
    files.client.c_str(),
    files.client_key.c_str());
  std::fflush(stdout);
  std::string first, second;
  check(static_cast<bool>(std::getline(std::cin, first)) && static_cast<bool>(std::getline(std::cin, second)));
  auto port = weave::parse_port(first);
  auto fallback = weave::parse_port(second);
  auto address = weave::IpAddress::parse("127.0.0.1");
  check(port.has_value() && fallback.has_value() && address.has_value());

  pg::Options options;
  options.host = "localhost";
  options.user = "weave";
  options.plaintext = plaintext;
  options.client_certificate = policy == "disable" ? weave::TlsCertificateMode::disable
    : policy == "require"                          ? weave::TlsCertificateMode::require
                                                   : weave::TlsCertificateMode::allow;
  options.tls_negotiation = negotiation;
  options.min_protocol = options.max_protocol = pg::ProtocolVersion::v30;
  options.connect_timeout = std::chrono::seconds{5};
  options.hosts = {{options.host, *port, *address}, {options.host, *fallback, *address}};
  weave::TlsClientOptions credentials;
  credentials.ca_file = files.ca;
  credentials.min_version = credentials.max_version = version;
  if (identity != "absent") {
    credentials.certificate_file = files.client;
    credentials.private_key_file = files.client_key;
  }
  if (identity == "ignored") {
    check(policy == "disable");
    credentials.certificate_file = (files.directory / "missing.pem").string();
    credentials.private_key_file = (files.directory / "missing.key").string();
    auto provider = weave::TlsPasswordProvider::create([](std::string_view) noexcept -> weave::Result<std::string> {
      ++providers;
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
    });
    check(provider.has_value());
    credentials.private_key_password_provider = *provider;
  }
  if (identity == "prebuilt") {
    auto context = weave::TlsContext::client(std::move(credentials));
    check(context.has_value());
    options.tls = *context;
  } else if (!plaintext) {
    options.tls_options = std::move(credentials);
  }
  if (identity == "oauth") {
    auto provider = pg::OAuthProvider::create([](pg::OAuthRequest request) noexcept -> weave::Task<pg::OAuthToken> {
      ++oauth_callbacks;
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
  const bool available = identity == "file" || identity == "prebuilt" || identity == "oauth";
  const bool success = policy != "require" || (available && request);

  auto ctx = weave::Context::create();
  check(ctx.has_value());
  check(ctx->run(session(options, success)).has_value());
  blocking(options, success);
  unsigned sessions = 2;
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto layout : support::io_layouts) {
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      check(runtime.has_value());
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned i = 0; i < 4; ++i) {
        auto job = runtime->spawn(session(options, success));
        check(job.has_value());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs)
        check(std::move(job).get().has_value());
      sessions += 4;
    }
  }
#endif
  check(providers == 0);
  check(oauth_callbacks == (identity == "oauth" && success ? sessions * 2 : 0));
  std::printf(
    "Certificate PostgreSQL: %u checks, %u sessions, %u providers, %u oauth\n",
    checks.load(),
    sessions,
    providers.load(),
    oauth_callbacks.load());
}
