#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/resolve.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <weave/timer.hpp>
#include <openssl/crypto.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks = 0;
static std::atomic<unsigned> resolved_endpoints = 0;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Report check failed at line %u\n", where.line());
    std::exit(EXIT_FAILURE);
  }
}

static pg::ConnectionReport sentinel()
{
  pg::ConnectionReport report;
  report.error = std::make_error_code(std::errc::permission_denied);
  report.truncated = true;
  return report;
}

static void format()
{
  pg::ConnectionReport report;
  check(report.format().has_value());
  check(report.format()->empty());
  auto endpoint = weave::Endpoint::parse("::1", 5432);
  check(endpoint.has_value());
  auto failure = std::make_error_code(std::errc::connection_refused);
  report.attempts.push_back(
    {.host = "bad\n\"host",
      .port = 5432,
      .endpoint = *endpoint,
      .stage = pg::ConnectionStage::transport,
      .error = failure});
  report.attempts.push_back(
    {.host = "localhost",
      .port = 5432,
      .stage = pg::ConnectionStage::authentication,
      .error = pg::sql_error("28000"),
      .diagnostic = {{{'S', "FATAL"}, {'C', "28000"}, {'M', "denied"}, {'D', "hidden detail"}}}});
  report.error = report.attempts.back().error;
  report.completed = true;
  auto minimal = report.format();
  check(minimal.has_value());
  check(minimal->find("bad\\x0a\\\"host") != std::string::npos);
  check(minimal->find("[::1]:5432") != std::string::npos);
  check(minimal->find("FATAL:  denied\n") != std::string::npos);
  check(minimal->find("hidden detail") == std::string::npos);
  auto full = report.format({.verbosity = pg::DiagnosticVerbosity::standard});
  check(full && full->find("hidden detail") != std::string::npos);
  auto copy = report;
  report.attempts.clear();
  check(copy.format() && *copy.format() == *minimal);
  check(!copy.format({.output_bytes = minimal->size() - 1}));
  check(copy.format({.output_bytes = minimal->size()}).has_value());
  check(!copy.format({.input_bytes = 1}));
  copy.attempts[0].stage = static_cast<pg::ConnectionStage>(-1);
  check(!copy.format());
  copy.attempts[0].stage = pg::ConnectionStage::transport;
  copy.attempts[0].error = {};
  check(!copy.format());
  copy.attempts[0].error = failure;
  copy.attempts[1].diagnostic.fields.emplace_back('M', "duplicate");
  check(!copy.format());
  copy.attempts[1].diagnostic.fields.pop_back();
  copy.error = {};
  copy.truncated = true;
  auto recovered = copy.format();
  check(recovered && recovered->find("Connection established.\n") != std::string::npos);
  check(recovered && recovered->find("history truncated") != std::string::npos);
  copy.attempts.resize(pg::ConnectionReport::max_attempts + 1);
  check(!copy.format());
}

static void verify(const pg::ConnectionReport &report, std::string_view mode, std::error_code error)
{
  check(report.completed);
  check(report.error == error);
  if (mode == "addresses_refused") {
    check(error != std::error_code{});
    check(!report.attempts.empty());
    for (const auto &attempt : report.attempts) {
      check(attempt.host == "localhost");
      check(attempt.stage == pg::ConnectionStage::transport);
      check(attempt.endpoint.has_value());
    }
  } else if (mode == "success" || mode == "reset_success") {
    check(!error);
    check(report.attempts.empty());
  } else if (mode == "fallback") {
    check(!error);
    check(report.attempts.size() == 1);
    check(report.attempts[0].stage == pg::ConnectionStage::transport);
  } else if (mode == "prefer") {
    check(!error);
    check(report.attempts.size() == 2);
    for (const auto &attempt : report.attempts) {
      check(attempt.stage == pg::ConnectionStage::target_session);
      check(attempt.error == pg::Error::target_session);
    }
  } else if (mode == "huge") {
    check(error == pg::sql_error("28000"));
    check(report.truncated);
    check(report.attempts.size() == 1);
    check(report.attempts[0].error == error);
    check(report.attempts[0].stage == pg::ConnectionStage::startup);
    check(report.attempts[0].diagnostic_truncated);
    check(report.attempts[0].diagnostic.fields.empty());
  } else {
    check(report.attempts.size() == (mode == "refused" || mode == "target_reject" ? 2u : 1u));
    auto expected = pg::ConnectionStage::startup;
    if (mode == "refused")
      expected = pg::ConnectionStage::transport;
    else if (mode == "gss" || mode == "gss_native")
      expected = pg::ConnectionStage::gss;
    else if (mode == "tls")
      expected = pg::ConnectionStage::tls;
    else if (mode == "password")
      expected = pg::ConnectionStage::authentication;
    else if (mode.starts_with("target"))
      expected = pg::ConnectionStage::target_session;
    else if (mode == "invalid" || mode == "busy")
      expected = pg::ConnectionStage::validation;
    for (const auto &attempt : report.attempts)
      check(attempt.stage == expected);
    if (mode == "sql" || mode == "target_sql" || mode == "target_protocol" || mode == "target_cancel_after_sql") {
      check(report.attempts.back().diagnostic.sqlstate() == (mode == "sql" ? "28000" : "42501"));
      check(report.attempts.back().diagnostic.message() == "denied");
    } else if (mode == "gss_native") {
      check(!report.attempts.back().diagnostic.message().empty());
    } else {
      check(report.attempts.back().diagnostic.fields.empty());
    }
    if (mode == "deadline")
      check(error == std::errc::timed_out);
    else if (mode == "cancel" || mode == "target_cancel_after_sql")
      check(error == std::errc::operation_canceled);
    else if (mode == "invalid")
      check(error == std::errc::invalid_argument);
    else if (mode == "busy")
      check(error == pg::Error::busy);
    else if (mode == "target_reject")
      check(error == pg::Error::target_session);
    else if (mode == "eof" || mode == "target_eof")
      check(error == std::errc::connection_reset);
    else if (mode == "malformed" || mode == "target_protocol")
      check(error == pg::Error::protocol);
  }
  check(report.truncated == (mode == "huge"));
  auto text = report.format();
  check(text.has_value());
  check(text->find("secret-pass-report") == std::string::npos);
}

static weave::Task<void> exercise(weave::Context &ctx, pg::Options options, std::string mode)
{
  auto report = sentinel();
  {
    auto dropped = pg::connect(options, report);
  }
  check(!report.completed && report.truncated && report.error == std::errc::permission_denied);

  std::vector<weave::Endpoint> resolved;
  if (mode == "addresses_refused")
    resolved = co_await weave::resolve(options.host, options.port);

  weave::CancelSource cancellation;
  auto operation = pg::connect(options, report, [&](const pg::Diagnostic &notice) noexcept {
    if (notice.message() == "CANCEL")
      cancellation.cancel();
  });
  std::optional<weave::JoinHandle<pg::Connection>> job;
  if (mode == "cancel" || mode == "target_cancel_after_sql") {
    auto submitted = ctx.spawn(std::move(operation), {.cancel = cancellation.token()});
    check(submitted.has_value());
    job.emplace(std::move(*submitted));
  }
  auto connection = job ? co_await weave::as_result(std::move(*job)) : co_await weave::as_result(std::move(operation));
  if (mode == "addresses_refused") {
    check(report.attempts.size() == resolved.size());
#if defined(_WIN32)
    check(report.attempts.size() >= 2);
#else
    check(!report.attempts.empty());
#endif
    resolved_endpoints += static_cast<unsigned>(resolved.size());
    for (std::size_t index = 0; index < resolved.size(); ++index)
      check(report.attempts[index].endpoint == resolved[index]);
  }
  if (mode.starts_with("reset") || mode == "busy") {
    check(connection.has_value());
    check(report.completed && !report.error);
    auto before = connection->configuration();
    check(before.has_value() && before->user == options.user);
    auto reset_options = options;
    reset_options.application_name = "new-reset-configuration";
    if (mode == "reset_fail")
      reset_options.user = "fail";
    auto reset_report = sentinel();
    {
      auto dropped = connection->reset(reset_options, reset_report);
    }
    check(!reset_report.completed && reset_report.truncated);
    if (mode == "busy") {
      auto deferred = connection->query("unused");
      auto reset = co_await weave::as_result(connection->reset(std::move(reset_options), reset_report));
      check(!reset && reset.error() == pg::Error::busy);
      verify(reset_report, "busy", reset.error());
      check(connection->open());
    } else {
      auto reset = co_await weave::as_result(connection->reset(std::move(reset_options), reset_report));
      verify(reset_report, mode == "reset_fail" ? "sql" : "reset_success", reset ? std::error_code{} : reset.error());
      check(connection->open() == (mode == "reset_success"));
      auto after = connection->configuration();
      check(after.has_value() && after->user == options.user);
      check(
        after->application_name == (mode == "reset_success" ? "new-reset-configuration" : options.application_name));
      check(before->application_name == options.application_name);
      check(connection->last_failure()->error == (reset ? std::error_code{} : reset.error()));
    }
    if (connection->open())
      co_await connection->finish();
    co_return;
  }
  verify(report, mode, connection ? std::error_code{} : connection.error());
  if (connection)
    co_await connection->finish();
}

int main(int argc, char **argv)
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  if (argc == 2 && std::string_view{argv[1]} == "format") {
    format();
  } else {
    check(argc == 5 || argc == 6);
    auto port = weave::parse_port(argv[1]);
    auto refused = weave::parse_port(argv[2]);
    check(port.has_value() && refused.has_value());
    std::string mode = argv[3];
    std::string_view engine = argv[4];
    pg::Options
      options{.host = "127.0.0.1", .port = *port, .user = "probe", .password = "secret-pass-report", .plaintext = true};
    if (mode == "addresses_refused") {
      options.host = "localhost";
      options.port = *refused;
    } else if (mode == "refused")
      options.hosts = {{"127.0.0.1", *refused}, {"127.0.0.1", *refused}};
    else if (mode == "fallback")
      options.hosts = {{"127.0.0.1", *refused}, {"127.0.0.1", *port}};
    else if (mode.starts_with("target") || mode == "prefer") {
      options.hosts = {{"127.0.0.1", *port}, {"127.0.0.1", *port}};
      options.target_session = mode == "prefer" ? pg::TargetSession::prefer_standby : pg::TargetSession::read_only;
    } else if (mode == "gss" || mode == "gss_native") {
      pg::GssContextOptions provider_options{.workers = 1, .capacity = 16};
      if (mode == "gss_native") {
        check(argc == 6);
        provider_options.credential_cache = argv[5];
      }
      auto provider = pg::GssContext::create(std::move(provider_options));
      check(provider.has_value());
      options.gss = *provider;
      options.gss_encryption = pg::GssEncryption::require;
      options.hosts = {{"127.0.0.1", *port}, {"127.0.0.1", *refused}};
    } else if (mode == "tls")
      options.plaintext = false;
    else if (mode == "invalid")
      options.user.clear();
    else if (mode == "deadline")
      options.connect_timeout = 100ms;

    if (engine == "blocking") {
      auto report = sentinel();
      auto connection = pg::BlockingConnection::connect(options, report);
      if (mode.starts_with("reset")) {
        check(connection.has_value());
        auto before = connection->configuration();
        check(before.has_value() && before->user == options.user);
        auto reset_options = options;
        reset_options.application_name = "new-reset-configuration";
        if (mode == "reset_fail")
          reset_options.user = "fail";
        auto reset = connection->reset(std::move(reset_options), report);
        verify(report, mode == "reset_fail" ? "sql" : "reset_success", reset ? std::error_code{} : reset.error());
        auto after = connection->configuration();
        check(after.has_value() && after->user == options.user);
        check(
          after->application_name == (mode == "reset_success" ? "new-reset-configuration" : options.application_name));
        check(before->application_name == options.application_name);
        check(connection->last_failure()->error == (reset ? std::error_code{} : reset.error()));
      } else {
        verify(report, mode, connection ? std::error_code{} : connection.error());
      }
      if (connection && connection->open())
        check(connection->finish().has_value());
    } else if (engine == "context") {
      auto ctx = weave::Context::create();
      check(ctx.has_value());
      auto result = ctx->run(exercise(*ctx, options, mode));
      if (!result)
        return weave::report_error(result.error());
    }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    else {
      weave::RuntimeOptions configuration{
        .workers = 4,
        .scheduler = engine == "affine" ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing};
#if defined(_WIN32)
      if (engine == "shared")
        configuration.io_layout = weave::IoLayout::shared;
#endif
      auto runtime = weave::Runtime::create(configuration);
      check(runtime.has_value());
      std::vector<weave::JoinHandle<void>> owned;
      for (unsigned i = 0; i < 16; ++i) {
        auto job = runtime->spawn([options, mode](weave::Context &ctx) {
          return exercise(ctx, options, mode);
        });
        check(job.has_value());
        owned.push_back(std::move(*job));
      }
      for (auto &job : owned)
        check(std::move(job).get().has_value());
    }
#endif
#if !defined(WEAVE_POSTGRES_TEST_RUNTIME)
    else
    {
      check(false);
    }
#endif
  }
  std::printf(
    "{\"checks\":%u,\"openssl\":%llu,\"resolved_endpoints\":%u}\n",
    checks.load(),
    static_cast<unsigned long long>(OpenSSL_version_num()),
    resolved_endpoints.load());
}
