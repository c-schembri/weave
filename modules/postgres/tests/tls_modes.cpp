#include <weave/postgres.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include "runtime_fixture.hpp"
#endif
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <source_location>

namespace pg = weave::pg;
static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> sessions{0};

static void check(bool condition, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "TLS mode check failed at %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

static bool rejected(std::string_view mode)
{
  return mode == "verify-full-wrong" || mode == "verify-ca-untrusted" || mode == "require-n" ||
    mode == "prefer-invalid" || mode == "prefer-auth-failure" || mode == "allow-auth-failure" ||
    mode == "prefer-timeout" || mode == "prefer-ca-untrusted" || mode.ends_with("-password");
}

static bool plaintext(std::string_view mode)
{
  return mode == "disable" || mode == "prefer-n" || mode == "prefer-broken" || mode == "allow";
}

template <class Connection>
static void connected(
  const weave::Result<Connection> &connection,
  const pg::ConnectionReport &report,
  std::string_view mode)
{
  ++sessions;
  if (!connection && !rejected(mode)) {
    std::fprintf(
      stderr,
      "Connection failed: %s (%d)\n",
      connection.error().message().c_str(),
      connection.error().value());
    auto formatted = report.format();
    if (formatted)
      std::fprintf(stderr, "%s\n", formatted->c_str());
  }
  check(connection.has_value() != rejected(mode));
  check(report.completed && report.current.elapsed.count() > 0);
  std::chrono::microseconds accounted{0};
  for (auto elapsed : report.current.stage_times) {
    check(elapsed.count() >= 0);
    accounted += elapsed;
  }
  check(accounted <= report.current.elapsed);
  if (!connection) {
    check(!report.attempts.empty() && report.error == connection.error());
    if (mode == "verify-full-wrong" || mode == "verify-ca-untrusted" || mode == "prefer-ca-untrusted")
      check(connection.error() == weave::TlsError::certificate_verification);
    if (mode == "prefer-timeout")
      check(connection.error() == std::errc::timed_out && report.current.error == std::errc::timed_out);
    if (mode.ends_with("auth-failure"))
      check(pg::sqlstate(connection.error()) == "28P01");
    return;
  }
  auto info = connection->info();
  check(info.has_value() && info->tls.has_value() != plaintext(mode));
  if (info->tls) {
    const bool trusted = mode == "verify-ca" || mode == "verify-full" || mode == "require-ca";
    check(info->tls->certificate_verified == trusted);
    check(info->tls->hostname_verified == (mode == "verify-full"));
    check(info->tls->peer.has_value() == trusted);
  }
  if (mode == "verify-full") {
    auto configuration = connection->configuration();
    check(configuration && configuration->tls_options && configuration->tls_options->key_logging);
  }
  const bool retried = mode == "allow-upgrade" || mode == "prefer-broken";
  check(report.attempts.size() == (retried ? 1 : 0));
  if (retried)
    check(report.attempts[0].endpoint == report.current.endpoint);
}

static weave::Task<void> session(pg::Options options, std::string mode)
{
  pg::ConnectionReport report;
  auto connection = co_await weave::as_result(pg::connect(std::move(options), report));
  connected(connection, report, mode);
  if (!connection)
    co_return;
  auto rows = co_await connection->query("SELECT 42");
  check(rows.size() == 1 && rows[0].rows.size() == 1 && rows[0].rows[0][0].bytes() == "42");
  co_await connection->request_cancel();
  co_await connection->finish();
}

static void blocking(pg::Options options, const std::string &mode)
{
  pg::ConnectionReport report;
  auto connection = pg::BlockingConnection::connect(std::move(options), report);
  connected(connection, report, mode);
  if (!connection)
    return;
  auto rows = connection->query("SELECT 42");
  check(rows && rows->size() == 1 && (*rows)[0].rows.size() == 1 && (*rows)[0].rows[0][0].bytes() == "42");
  auto cancellation = connection->cancel_handle();
  check(cancellation.has_value() && cancellation->request_blocking().has_value());
  check(connection->finish().has_value());
}

static void exercise(const pg::Options &options, const std::string &mode)
{
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  auto result = ctx->run(session(options, mode));
  if (!result)
    std::fprintf(
      stderr,
      "%s: %s (%s:%d)\n",
      mode.c_str(),
      result.error().message().c_str(),
      result.error().category().name(),
      result.error().value());
  check(result.has_value());
  blocking(options, mode);
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto layout : support::io_layouts) {
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      check(runtime.has_value());
      std::vector<weave::JoinHandle<void>> jobs;
      for (int index = 0; index < 4; ++index) {
        auto job = runtime->spawn(session(options, mode));
        check(job.has_value());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs) {
        auto outcome = std::move(job).get();
        if (!outcome)
          std::fprintf(
            stderr,
            "%s worker: %s (%s:%d)\n",
            mode.c_str(),
            outcome.error().message().c_str(),
            outcome.error().category().name(),
            outcome.error().value());
        check(outcome.has_value());
      }
    }
  }
#endif
}

static pg::Options configure(fixture::Certificates &files, weave::u16 port, const std::string &mode)
{
  auto separator = mode.find('-');
  std::string policy = mode.substr(0, separator);
  if (mode.starts_with("verify-full"))
    policy = "verify-full";
  else if (mode.starts_with("verify-ca"))
    policy = "verify-ca";
  auto options = pg::Options::parse("user=weave dbname=postgres sslmode=" + policy);
  check(options.has_value());
  options->host = mode == "verify-full-wrong" || mode == "verify-ca" || mode == "require-ca" ? "wrong.invalid"
                                                                                             : "localhost";
  auto address = weave::IpAddress::parse("127.0.0.1");
  check(address.has_value());
  options->hosts = {{options->host, port, *address}};
  options->connect_timeout = mode == "prefer-timeout" ? std::chrono::milliseconds{100} : std::chrono::seconds{3};
  if (mode.ends_with("-password")) {
    options->allow_cleartext_password = true;
    options->password = "must-never-be-sent";
  }
  if (policy.starts_with("verify") || mode == "require-ca" || mode == "prefer-ca-untrusted") {
    if (!options->tls_options)
      options->tls_options.emplace();
    options->tls_options->ca_file = mode.ends_with("-untrusted") ? files.untrusted : files.ca;
    if (mode == "verify-full")
      options->tls_options->key_log_file = (files.directory / "postgres.keys").string();
  }

  return std::move(*options);
}

int main(int argc, char **argv)
{
  check(argc == 1 || argc == 2);
  fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string first;
  check(static_cast<bool>(std::getline(std::cin, first)));
  auto port = weave::parse_port(first);
  check(port.has_value());
  if (argc == 2) {
    exercise(configure(files, *port, argv[1]), argv[1]);
  } else {
    std::array<std::string, 4> configuration;
    for (auto &field : configuration)
      check(static_cast<bool>(std::getline(std::cin, field)));
    const std::array modes{
      "disable",
      "allow",
      "prefer",
      "require",
      "require-ca",
      "verify-ca",
      "verify-full",
      "verify-ca-untrusted",
      "verify-full-wrong"};
    for (const auto *mode : modes) {
      auto options = configure(files, *port, mode);
      options.password = configuration[0];
      if (options.tls_mode != pg::TlsMode::disable) {
        if (!options.tls_options)
          options.tls_options.emplace();
        options.tls_options->certificate_file = files.client;
        options.tls_options->private_key_file = files.client_key;
      }
      exercise(options, mode);
    }
  }
  const auto log = files.directory / "postgres.keys";
  std::error_code error;
  if (argc == 1 || std::string_view{argv[1]} == "verify-full") {
    check(std::filesystem::file_size(log, error) > 0 && !error);
    check(std::filesystem::remove(log, error) && !error);
  }
  std::printf("TLS modes: %u sessions, %u checks\n", sessions.load(), checks.load());
}
