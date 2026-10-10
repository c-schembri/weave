#include <weave/postgres.hpp>
#include <weave/scope.hpp>
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include "runtime_fixture.hpp"
#endif
#include <iostream>
#include <atomic>
#include <source_location>
#include <cstdio>
#include <thread>

namespace pg = weave::pg;
using Mode = weave::TlsCertificateMode;

static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> cancellations{0};
static std::atomic<unsigned> sessions{0};
static std::atomic<unsigned> stale_requests{0};
static std::atomic<unsigned> providers{0};
static weave::TlsVersion expected_version;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Real certificate control failed: %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

static void completed(weave::Result<void> result)
{
  if (!result)
    std::fprintf(
      stderr,
      "Root failure: category=%s value=%d message=%s\n",
      result.error().category().name(),
      result.error().value(),
      result.error().message().c_str());
  check(result.has_value());
}

static std::string scalar(const pg::Results &results, std::size_t column = 0)
{
  check(results.size() == 1 && results.front().rows.size() == 1);
  check(results.front().rows.front().size() > column);
  return std::string(results.front().rows.front()[column].bytes());
}

static bool sends_identity(const pg::Options &options, bool requests)
{
  if (!requests || options.client_certificate == Mode::disable)
    return false;
  return options.tls.has_value() || (options.tls_options && !options.tls_options->certificate_file.empty());
}

static void metadata(pg::ConnectionInfo info, pg::OptionsInfo snapshot, const pg::Options &options)
{
  check(info.tls.has_value());
  check(info.tls->version == expected_version);
  check(info.authentication.complete && info.authentication.method == pg::Authentication::scram_sha256);
  check(snapshot.client_certificate == options.client_certificate);
  check(snapshot.tls_negotiation == options.tls_negotiation);
  if (options.tls_negotiation == pg::TlsNegotiation::direct)
    check(info.tls->negotiated_protocol == "postgresql");
}

static weave::Task<void> cancel_after_notice(pg::Connection &connection)
{
  for (unsigned attempt = 0; attempt < 300; ++attempt) {
    auto notices = connection.take_notices();
    for (const auto &notice : notices) {
      if (notice.message() == "certificate_cancel_ready") {
        auto canceled = co_await weave::as_result(connection.request_cancel());
        if (!canceled) {
          std::fprintf(
            stderr,
            "Async cancel failure: category=%s value=%d message=%s\n",
            canceled.error().category().name(),
            canceled.error().value(),
            canceled.error().message().c_str());
          co_await weave::fail(canceled.error());
        }
        ++cancellations;
        co_return;
      }
    }
    co_await weave::sleep_for(std::chrono::milliseconds{10});
  }
  co_await weave::fail(std::errc::timed_out);
}

static weave::Task<void> slow_query(pg::Connection &connection)
{
  auto result = co_await weave::as_result(
    connection.query("DO $$BEGIN RAISE NOTICE 'certificate_cancel_ready'; PERFORM pg_sleep(10); END$$"));
  check(!result && pg::sqlstate(result.error()) == "57014");
}

static weave::Task<void> stale_after_notice(
  pg::Connection &connection,
  pg::CancelHandle snapshot,
  std::atomic<bool> &dispatched)
{
  for (unsigned attempt = 0; attempt < 300; ++attempt) {
    for (const auto &notice : connection.take_notices()) {
      if (notice.message() == "certificate_stale_ready") {
        co_await snapshot.request();
        dispatched = true;
        co_return;
      }
    }
    co_await weave::sleep_for(std::chrono::milliseconds{10});
  }
  co_await weave::fail(std::errc::timed_out);
}

static weave::Task<void> stale_query(pg::Connection &connection, std::atomic<bool> &dispatched)
{
  co_await connection.query("DO $$BEGIN RAISE NOTICE 'certificate_stale_ready'; PERFORM pg_sleep(0.5); END$$");
  check(dispatched.load());
  ++stale_requests;
}

static void reset_policy(pg::Options &options)
{
  options.client_certificate = options.client_certificate == Mode::disable ? Mode::allow : Mode::disable;
  if (options.tls_options && options.tls_options->certificate_file.ends_with("missing.pem")) {
    options.tls_options->certificate_file.clear();
    options.tls_options->private_key_file.clear();
    options.tls_options->private_key_password_provider.reset();
  }
}

static weave::Task<void> session(pg::Options options, bool requests, bool success, bool strict = false)
{
  ++sessions;
  pg::ConnectionReport report;
  auto connection = co_await weave::as_result(pg::connect(options, report));
  if (!success) {
    if (connection)
      std::fprintf(
        stderr,
        "Unexpected successful connection: mode=%u strict=%u request=%u\n",
        static_cast<unsigned>(options.client_certificate),
        strict,
        requests);
    check(!connection);
    check(report.completed && report.attempts.size() == 1);
    if (strict)
      check(pg::sqlstate(connection.error()) == "28000");
    else
      check(connection.error() == weave::TlsError::client_certificate_required);
    co_return;
  }

  if (!connection)
    std::fprintf(stderr, "Connect failed: %s\n", connection.error().message().c_str());
  check(connection.has_value());
  auto info = connection->info();
  auto snapshot = connection->configuration();
  check(info.has_value() && snapshot.has_value());
  metadata(*info, *snapshot, options);
  auto rows = co_await connection->query(
    "SELECT ssl::text, (client_dn IS NOT NULL)::text FROM pg_stat_ssl WHERE pid=pg_backend_pid()");
  check(scalar(rows) == "true");
  check(scalar(rows, 1) == (sends_identity(options, requests) ? "true" : "false"));
  auto old = connection->cancel_handle();
  check(old.has_value());
  auto process = connection->backend_process();

  co_await weave::timeout(
    std::chrono::seconds{5},
    weave::when_all(slow_query(*connection), cancel_after_notice(*connection)));
  check(scalar(co_await connection->query("SELECT 42")) == "42");
  if (!strict) {
    reset_policy(options);
    co_await connection->reset(options);
    check(connection->backend_process() != process);
    info = connection->info();
    snapshot = connection->configuration();
    check(info.has_value() && snapshot.has_value());
    metadata(*info, *snapshot, options);
    rows = co_await connection->query(
      "SELECT ssl::text, (client_dn IS NOT NULL)::text FROM pg_stat_ssl WHERE pid=pg_backend_pid()");
    check(scalar(rows) == "true");
    check(scalar(rows, 1) == (sends_identity(options, requests) ? "true" : "false"));
    std::atomic<bool> dispatched{false};
    co_await weave::timeout(
      std::chrono::seconds{5},
      weave::when_all(stale_query(*connection, dispatched), stale_after_notice(*connection, *old, dispatched)));
    check(scalar(co_await connection->query("SELECT 43")) == "43");
    co_await weave::timeout(
      std::chrono::seconds{5},
      weave::when_all(slow_query(*connection), cancel_after_notice(*connection)));
    check(scalar(co_await connection->query("SELECT 44")) == "44");
  }
  auto finished = co_await weave::as_result(connection->finish());
  if (!finished) {
    std::fprintf(
      stderr,
      "Session finish failure: category=%s value=%d message=%s\n",
      finished.error().category().name(),
      finished.error().value(),
      finished.error().message().c_str());
    co_await weave::fail(finished.error());
  }
}

static void blocking_cancel(
  pg::BlockingConnection &connection,
  pg::Options options,
  const pg::CancelHandle *snapshot = nullptr)
{
  auto handle = connection.cancel_handle();
  check(handle.has_value());
  if (snapshot)
    *handle = *snapshot;
  auto process = connection.backend_process();
  std::atomic<bool> dispatched{false};
  std::optional<std::error_code> failure;
  const char *phase = "not started";
  std::thread worker{[handle = *handle, options, process, &dispatched, &failure, &phase] {
    phase = "observer connect";
    auto observer = pg::BlockingConnection::connect(options);
    if (!observer) {
      failure = observer.error();
      return;
    }
    const auto sql = "SELECT EXISTS (SELECT 1 FROM pg_stat_activity WHERE pid=" + std::to_string(process) +
      " AND state='active' AND wait_event='PgSleep')";
    for (unsigned attempt = 0; attempt < 300; ++attempt) {
      phase = "observer query";
      auto active = observer->query(sql);
      if (!active) {
        failure = active.error();
        return;
      }
      if (scalar(*active) == "t") {
        phase = "cancel dispatch";
        auto status = handle.request_blocking();
        if (!status)
          failure = status.error();
        else
          dispatched = true;
        if (!failure)
          phase = "observer finish";
        auto finish = observer->finish();
        if (!finish && !failure)
          failure = finish.error();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    failure = std::make_error_code(std::errc::timed_out);
  }};

  const char *sql = snapshot ? "SELECT pg_sleep(0.5), 42" : "SELECT pg_sleep(10)";
  auto result = connection.query(sql);
  bool dispatched_before_completion = dispatched.load();
  worker.join();
  if (!dispatched || failure) {
    std::fprintf(
      stderr,
      "Blocking cancellation: dispatched=%u phase=%s target=%s\n",
      dispatched.load(),
      phase,
      result ? "successful query" : result.error().message().c_str());
    if (failure)
      std::fprintf(
        stderr,
        "Worker failure: category=%s value=%d message=%s\n",
        failure->category().name(),
        failure->value(),
        failure->message().c_str());
  }
  check(dispatched && !failure);
  if (snapshot) {
    check(dispatched_before_completion && result.has_value());
    check(scalar(*result, 1) == "42");
    ++stale_requests;
  } else {
    check(!result && pg::sqlstate(result.error()) == "57014");
    ++cancellations;
  }
}

static void blocking(pg::Options options, bool requests, bool success)
{
  ++sessions;
  pg::ConnectionReport report;
  auto connection = pg::BlockingConnection::connect(options, report);
  if (!success) {
    check(!connection && connection.error() == weave::TlsError::client_certificate_required);
    check(report.completed && report.attempts.size() == 1);
    return;
  }
  check(connection.has_value());
  auto info = connection->info();
  auto snapshot = connection->configuration();
  check(info.has_value() && snapshot.has_value());
  metadata(*info, *snapshot, options);
  auto rows = connection->query(
    "SELECT ssl::text, (client_dn IS NOT NULL)::text FROM pg_stat_ssl WHERE pid=pg_backend_pid()");
  check(rows.has_value());
  check(scalar(*rows, 1) == (sends_identity(options, requests) ? "true" : "false"));
  auto original = connection->cancel_handle();
  check(original.has_value());
  blocking_cancel(*connection, options);
  reset_policy(options);
  check(connection->reset(options).has_value());
  snapshot = connection->configuration();
  check(snapshot.has_value() && snapshot->client_certificate == options.client_certificate);
  rows = connection->query(
    "SELECT ssl::text, (client_dn IS NOT NULL)::text FROM pg_stat_ssl WHERE pid=pg_backend_pid()");
  check(rows.has_value());
  check(scalar(*rows, 1) == (sends_identity(options, requests) ? "true" : "false"));
  blocking_cancel(*connection, options, &*original);
  blocking_cancel(*connection, options);
  rows = connection->query("SELECT 45");
  check(rows.has_value() && scalar(*rows) == "45");
  check(connection->finish().has_value());
}

int main()
{
  static fixture::Certificates files;
  if (OpenSSL_version_num() != OPENSSL_VERSION_NUMBER)
    std::fprintf(stderr, "OpenSSL mismatch: runtime=%lx headers=%lx\n", OpenSSL_version_num(), OPENSSL_VERSION_NUMBER);
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string port_text, password, request_text, version_text;
  check(static_cast<bool>(std::getline(std::cin, port_text)));
  check(static_cast<bool>(std::getline(std::cin, password)));
  check(static_cast<bool>(std::getline(std::cin, request_text)));
  check(static_cast<bool>(std::getline(std::cin, version_text)));
  auto port = weave::parse_port(port_text);
  auto address = weave::IpAddress::parse("127.0.0.1");
  check(port.has_value() && address.has_value());
  bool requests = request_text == "1";
  auto version = version_text == "12" ? weave::TlsVersion::tls12 : weave::TlsVersion::tls13;
  expected_version = version;
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  const std::array negotiations{pg::TlsNegotiation::postgres, pg::TlsNegotiation::direct};
  const std::array modes{Mode::disable, Mode::allow, Mode::require};
  const std::array identities{"file", "prebuilt", "absent", "ignored"};
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
#endif
  unsigned successes = 0;
  unsigned expected_cancellations = 0;
  unsigned cases = 0;
  for (auto negotiation : negotiations) {
    for (auto mode : modes) {
      for (std::string_view identity : identities) {
        if (identity == "ignored" && mode != Mode::disable)
          continue;
        pg::Options options;
        options.host = "localhost";
        options.port = *port;
        options.hosts = {{"localhost", *port, *address}, {"localhost", *port, *address}};
        options.user = "weave";
        options.database = "postgres";
        options.password = password;
        options.channel_binding = pg::ChannelBinding::require;
        options.tls_negotiation = negotiation;
        options.client_certificate = mode;
        options.connect_timeout = std::chrono::seconds{5};
        weave::TlsClientOptions tls;
        tls.ca_file = files.ca;
        tls.min_version = tls.max_version = version;
        if (identity != "absent") {
          tls.certificate_file = files.client;
          tls.private_key_file = files.client_key;
        }
        if (identity == "ignored") {
          tls.certificate_file = (files.directory / "missing.pem").string();
          tls.private_key_file = (files.directory / "missing.key").string();
          auto provider = weave::TlsPasswordProvider::create(
            [](std::string_view) noexcept -> weave::Result<std::string> {
              ++providers;
              return std::unexpected(std::make_error_code(std::errc::permission_denied));
            });
          check(provider.has_value());
          tls.private_key_password_provider = *provider;
        }
        if (identity == "prebuilt") {
          auto credentials = weave::TlsContext::client(std::move(tls));
          check(credentials.has_value());
          options.tls = *credentials;
        } else {
          options.tls_options = std::move(tls);
        }
        bool success = mode != Mode::require || (requests && identity != "absent");
        std::printf(
          "Case: request=%u version=%s negotiation=%u mode=%u identity=%s success=%u\n",
          requests,
          version_text.c_str(),
          static_cast<unsigned>(negotiation),
          static_cast<unsigned>(mode),
          std::string(identity).c_str(),
          success);
        std::fflush(stdout);
        completed(ctx->run(session(options, requests, success)));
        blocking(options, requests, success);
        ++cases;
        if (success) {
          ++successes;
          expected_cancellations += 4;
        }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
        if (identity == "prebuilt" && success) {
          for (auto layout : support::io_layouts) {
            for (auto scheduler : schedulers) {
              auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
              check(runtime.has_value());
              std::vector<weave::JoinHandle<void>> jobs;
              for (unsigned index = 0; index < 4; ++index) {
                auto job = runtime->spawn(session(options, requests, true));
                check(job.has_value());
                jobs.push_back(std::move(*job));
              }
              for (auto &job : jobs)
                completed(std::move(job).get());
              expected_cancellations += 8;
            }
          }
        }
#endif
        if (requests && identity == "file" && mode != Mode::require) {
          options.user = "weave_mtls";
          bool strict_success = mode == Mode::allow;
          completed(ctx->run(session(options, requests, strict_success, true)));
          if (strict_success)
            ++expected_cancellations;
        }
      }
    }
  }
  check(cases == 20 && successes > 0);
  check(cancellations == expected_cancellations && providers == 0);
  std::printf(
    "Real certificate policy: %u checks, %u cases, %u sessions, %u observed query cancellations, "
    "%u active stale-request controls\n",
    checks.load(),
    cases,
    sessions.load(),
    cancellations.load(),
    stale_requests.load());
}
