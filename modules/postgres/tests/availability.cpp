#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#if defined(WEAVE_POSTGRES_TEST_LIBPQ_VERSION)
#include <libpq-fe.h>
#endif
#include "tls_certificates.hpp"
#include <openssl/err.h>
#include <openssl/sslerr.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace pg = weave::pg;
static std::atomic<unsigned> checks{0};
static std::atomic<unsigned> oauth_calls{0};

static weave::Task<pg::OAuthToken> unused_token(pg::OAuthRequest)
{
  co_await weave::fail(std::errc::operation_not_supported);
}

static void check(bool condition)
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Check failed: %u\n", checks.load());
    std::_Exit(EXIT_FAILURE);
  }
}

static void outcome(const weave::Result<pg::ServerStatus> &result, std::string_view mode)
{
  if (mode == "cancel" || mode == "pre_cancel" || mode == "cancel_recovery") {
    check(!result && result.error() == std::errc::operation_canceled);
  } else if (mode.starts_with("invalid_")) {
    check(!result && result.error() == std::errc::invalid_argument);
  } else if (mode == "ssl_refusal" || mode == "ssl_refusal_ready") {
    check(!result && result.error() == pg::make_error_code(pg::Error::authentication));
  } else if (mode == "untrusted" || mode == "expired" || mode == "recover_tls_expired" ||
    mode == "secure_failure_ready") {
    check(!result && result.error() == weave::make_error_code(weave::TlsError::certificate_verification));
  } else if (mode == "mtls_missing") {
    if (result)
      check(*result == pg::ServerStatus::no_response);
    else {
      check(std::string_view{result.error().category().name()} == "openssl");
      check(ERR_GET_REASON(static_cast<unsigned>(result.error().value())) == SSL_R_TLSV13_ALERT_CERTIFICATE_REQUIRED);
    }
  } else if (mode == "unknown_auth") {
    check(!result && result.error() == pg::make_error_code(pg::Error::unsupported_authentication));
  } else if (mode == "huge_frame" || mode == "resource_ready") {
    check(!result && result.error() == pg::make_error_code(pg::Error::resource_limit));
  } else if (mode.starts_with("malformed_") || mode == "unterminated_sasl" || mode == "empty_sasl" ||
    mode == "bad_sasl" || mode == "sasl_trailing" || mode == "unsolicited_continue" || mode == "ready_first" ||
    mode == "wrong_sqlstate" || mode == "bad_version" || mode == "recover_protocol" || mode == "protocol_ready") {
    check(!result && result.error() == pg::make_error_code(pg::Error::protocol));
  } else {
    if (!result)
      std::fprintf(stderr, "%s: %s\n", std::string{mode}.c_str(), result.error().message().c_str());
    check(result.has_value());
    auto expected = pg::ServerStatus::accepting;
    if (mode == "reject" || mode == "recover_reject")
      expected = pg::ServerStatus::rejecting;
    if (mode == "eof" || mode == "silent" || mode == "short_header" || mode == "short_body" ||
      mode == "timeout_header" || mode == "timeout_body" || mode == "refused" || mode == "recover_refused" ||
      mode == "recover_eof")
      expected = pg::ServerStatus::no_response;
    check(*result == expected);
  }
}

static weave::Task<void> exercise(weave::Context &ctx, pg::Options options, std::string mode)
{
  if (mode == "unstarted") {
    auto pending = pg::ping(std::move(options));
    co_return;
  }
  if (mode == "cancel" || mode == "pre_cancel" || mode == "cancel_recovery") {
    weave::CancelSource source;
    if (mode == "pre_cancel")
      source.cancel();
    auto child = ctx.spawn(pg::ping(std::move(options)), {.cancel = source.token()});
    check(child.has_value());
    if (mode == "cancel" || mode == "cancel_recovery") {
      co_await weave::sleep_for(std::chrono::milliseconds{50});
      source.cancel();
    }
    outcome(co_await weave::as_result(std::move(*child)), mode);
  } else {
    outcome(co_await weave::as_result(pg::ping(std::move(options))), mode);
  }
}

int main(int argc, char **argv)
{
  if (argc == 2 && std::string_view{argv[1]} == "fixture") {
    fixture::Certificates certificates;
    const std::array paths{
      certificates.ca,
      certificates.leaf,
      certificates.private_key,
      certificates.client,
      certificates.client_key,
      certificates.untrusted,
      certificates.expired};
    for (const auto &path : paths)
      std::printf("%s\n", path.c_str());
    std::fflush(stdout);
    check(std::getchar() == '\n');
    return EXIT_SUCCESS;
  }
  check(argc == 10);
  auto port = weave::parse_port(argv[1]);
  auto closed_port = weave::parse_port(argv[5]);
  auto next_port = weave::parse_port(argv[9]);
  check(port.has_value() && closed_port.has_value() && next_port.has_value());
  std::string mode = argv[2];
  std::string_view engine = argv[3];
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "probe", .plaintext = true};
  options.password = std::string(96, 's');
  options.connect_timeout = std::chrono::milliseconds{200};
  options.limits.message_bytes = 1024;
  options.target_session = pg::TargetSession::read_only;
  if (mode == "invalid_user")
    options.user.clear();
  if (mode == "invalid_port")
    options.port = 0;
  if (mode == "invalid_policy")
    options.target_session = static_cast<pg::TargetSession>(99);
  bool secured = mode == "tls12" || mode == "tls13" || mode == "tls_oauth" || mode.starts_with("mtls") ||
    mode == "untrusted" || mode == "expired" || mode.starts_with("recover_tls_") || mode == "secure_failure_ready" ||
    mode == "ssl_refusal_ready";
  if (mode == "ssl_refusal" || secured) {
    options.plaintext = false;
    options.connect_timeout = std::chrono::seconds{3};
    weave::TlsClientOptions tls{.ca_file = argv[6]};
    if (mode == "mtls12" || mode == "mtls13") {
      tls.certificate_file = argv[7];
      tls.private_key_file = argv[8];
    }
    auto version = mode.ends_with("12") ? weave::TlsVersion::tls12 : weave::TlsVersion::tls13;
    tls.min_version = version;
    tls.max_version = version;
    options.tls_options = std::move(tls);
  }
  if (mode == "refused")
    options.port = *closed_port;
  if (mode == "retry")
    options.hosts = {{"127.0.0.1", *closed_port}, {"127.0.0.1", *port}};

  bool multiple = mode.starts_with("recover_") || mode == "eof_ready" || mode == "random_ready" ||
    mode == "timeout_ready" || mode == "cancel_recovery" || mode == "protocol_ready" || mode == "resource_ready" ||
    mode == "secure_failure_ready" || mode == "ssl_refusal_ready";
  auto second_port = mode == "recover_refused" ? *closed_port : *next_port;
  if (multiple)
    options.hosts = {{"127.0.0.1", *port}, {"127.0.0.1", second_port}};
  if (mode == "random_ready")
    options.host_balance = pg::HostBalance::random;

  if (mode == "tls_oauth") {
    auto provider = pg::OAuthProvider::create([](pg::OAuthRequest request) noexcept {
      ++oauth_calls;
      return unused_token(std::move(request));
    });
    check(provider.has_value());
    options.oauth = pg::OAuthOptions{
      .issuer = "https://issuer.example",
      .client_id = "probe",
      .provider = std::move(*provider)};
  }

  if (engine == "native") {
#if defined(WEAVE_POSTGRES_TEST_LIBPQ_VERSION)
    check(std::atoi(argv[4]) == WEAVE_POSTGRES_TEST_LIBPQ_VERSION);
    check(PQlibVersion() == std::atoi(argv[4]));
    auto ports = mode == "retry" ? std::to_string(*closed_port) + "," + std::to_string(*port)
                                 : std::to_string(mode == "refused" ? *closed_port : *port);
    if (multiple)
      ports = std::to_string(*port) + "," + std::to_string(second_port);
    const std::array<const char *, 14> keys{
      "host",
      "port",
      "user",
      "dbname",
      "password",
      "sslmode",
      "gssencmode",
      "connect_timeout",
      "target_session_attrs",
      "sslrootcert",
      "sslcert",
      "sslkey",
      "sslcertmode",
      nullptr};
    bool mutual = mode == "mtls12" || mode == "mtls13";
    const std::array<const char *, 14> values{
      mode == "retry" || multiple ? "127.0.0.1,127.0.0.1" : "127.0.0.1",
      ports.c_str(),
      "probe",
      "probe",
      "",
      secured || mode == "ssl_refusal" ? "verify-full" : "disable",
      "disable",
      "3",
      "any",
      argv[6],
      mutual ? argv[7] : "",
      mutual ? argv[8] : "",
      mutual ? "require" : "disable",
      nullptr};
    auto result = PQpingParams(keys.data(), values.data(), 0);
    if (mode == "reject" || mode == "recover_reject")
      check(result == PQPING_REJECT);
    else if (mode == "refused" || mode == "eof" || mode == "ssl_refusal" || mode == "untrusted" || mode == "expired" ||
      mode == "mtls_missing" || mode == "recover_refused" || mode == "recover_eof" || mode == "recover_tls_expired" ||
      mode == "eof_ready")
      check(result == PQPING_NO_RESPONSE);
    else
      check(result == PQPING_OK);
#else
    check(false);
#endif
  } else if (engine == "blocking") {
    if (mode == "unstarted") {
      auto pending = pg::ping(std::move(options));
    } else {
      outcome(pg::ping_blocking(std::move(options)), mode);
    }
  } else if (engine == "context") {
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    check(ctx->run(exercise(*ctx, options, mode)).has_value());
    check(ctx->metrics().submitted == ctx->metrics().completed);
  } else {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    bool stealing = engine == "stealing" || engine == "shared_stealing";
    auto runtime = weave::Runtime::create(
      {.workers = 4,
        .scheduler = stealing ? weave::Scheduler::work_stealing : weave::Scheduler::worker_affine,
        .io_layout = engine.starts_with("shared_") ? weave::IoLayout::shared : weave::IoLayout::sharded});
    check(runtime.has_value());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn([options, mode](weave::Context &ctx) {
        return exercise(ctx, options, mode);
      });
      check(job.has_value());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      check(std::move(job).get().has_value());
#else
    check(false);
#endif
  }
  check(oauth_calls == 0);
  std::printf("Health controls passed: %u checks\n", checks.load());
}
