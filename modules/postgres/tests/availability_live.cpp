#include <weave/postgres.hpp>
#include <weave/timer.hpp>
#include <weave/port.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif

#include "tls_certificates.hpp"

#include <openssl/crypto.h>

#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace pg = weave::pg;
using namespace std::chrono_literals;

static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Live availability check failed at line %u\n", location.line());
    std::_Exit(EXIT_FAILURE);
  }
}

static void outcome(const weave::Result<pg::ServerStatus> &result, std::string_view mode)
{
  if (result)
    std::printf("Probe status: %d\n", static_cast<int>(*result));
  else
    std::printf("Probe error: %s / %d\n", result.error().category().name(), result.error().value());
  std::fflush(stdout);

  if (mode == "untrusted") {
    check(!result && result.error() == weave::TlsError::certificate_verification);
  } else if (mode == "local_wrong_user") {
    check(!result && result.error() == pg::Error::authentication);
  } else if (mode == "gss_missing" || mode == "gss_wrong_service") {
    check(!result && std::string_view{result.error().category().name()} == "weave.postgres.gssapi");
  } else if (mode == "gss_deadline") {
    check(!result && result.error() == std::errc::timed_out);
  } else if (mode == "pre_cancel" || mode.ends_with("_cancel")) {
    check(!result && result.error() == std::errc::operation_canceled);
  } else {
    check(result.has_value());
    auto expected = mode == "refused" ? pg::ServerStatus::no_response : pg::ServerStatus::accepting;
    check(*result == expected);
  }
}

static weave::Task<void> cancel_entered(weave::CancelSource &source, std::string mode)
{
#if !defined(_WIN32)
  using Entered = unsigned (*)();
  const char *symbol = "weave_test_gss_entered";
  if (mode == "gss_wrap_cancel")
    symbol = "weave_test_gss_wrapped";
  else if (mode == "gss_unwrap_cancel")
    symbol = "weave_test_gss_unwrapped";

  auto entered = reinterpret_cast<Entered>(dlsym(RTLD_DEFAULT, symbol));
  check(entered != nullptr);
  for (unsigned index = 0; index < 1000; ++index) {
    if (entered()) {
      source.cancel();
      co_return;
    }
    co_await weave::sleep_for(1ms);
  }
#endif
  source.cancel();
  co_await weave::fail(std::errc::timed_out);
}

static weave::Task<void> probing(pg::Options options, std::string mode)
{
  outcome(co_await weave::as_result(pg::ping(std::move(options))), mode);
}

static weave::Task<void> exercise(weave::Context &ctx, pg::Options options, std::string mode)
{
  if (mode == "unstarted") {
    auto pending = pg::ping(std::move(options));
    co_return;
  }

  if (mode == "pre_cancel" || mode.ends_with("_cancel")) {
    weave::CancelSource source;
    if (mode == "pre_cancel")
      source.cancel();

    auto job = ctx.spawn(pg::ping(std::move(options)), {.cancel = source.token()});
    check(job.has_value());
    auto before = std::chrono::steady_clock::now();
    if (mode != "pre_cancel")
      co_await cancel_entered(source, mode);

    outcome(co_await weave::as_result(std::move(*job)), mode);
    if (mode == "gss_cancel")
      check(std::chrono::steady_clock::now() - before >= 150ms);
    if (mode == "gss_wrap_cancel" || mode == "gss_unwrap_cancel")
      check(std::chrono::steady_clock::now() - before >= 80ms);
  } else {
    auto before = std::chrono::steady_clock::now();
    unsigned repetitions = mode == "gss_reuse" ? 4 : 1;
    for (unsigned index = 0; index < repetitions; ++index)
      co_await probing(options, mode);
    if (mode == "gss_deadline")
      check(std::chrono::steady_clock::now() - before >= 150ms);
  }
}

int main(int argc, char **argv)
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
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
    return 0;
  }

  check(argc == 13);
  auto port = weave::parse_port(argv[1]);
  auto closed_port = weave::parse_port(argv[4]);
  auto forbidden_port = weave::parse_port(argv[12]);
  auto address = weave::IpAddress::parse(argv[9]);
  check(port && closed_port && forbidden_port && address);

  std::string mode = argv[2];
  std::string_view engine = argv[3];
  pg::Options options{.host = "localhost", .port = *port, .user = argv[8], .database = "postgres", .plaintext = true};
  options.password = std::string(96, 's');
  options.application_name = "health_" + mode + "_" + std::string{engine};
  options.hosts = {{"localhost", *port, *address}};
  options.target_session = pg::TargetSession::read_only;
  options.connect_timeout = 3s;

  const bool secured = mode == "tls" || mode == "password" || mode == "mtls" || mode == "untrusted" ||
    mode == "no_certificate";
  if (secured) {
    options.plaintext = false;
    weave::TlsClientOptions tls{.ca_file = argv[5]};
    if (mode != "no_certificate") {
      tls.certificate_file = argv[6];
      tls.private_key_file = argv[7];
    }
    options.tls_options = std::move(tls);
  }

  if (mode == "wrong_database")
    options.database = "absent_database";
  if (mode == "refused") {
    options.port = *closed_port;
    options.hosts[0].port = *closed_port;
  }
  if (mode == "retry")
    options.hosts.insert(options.hosts.begin(), {"localhost", *closed_port, *address});

  if (mode.starts_with("local")) {
    options.host = argv[10];
    options.hosts = {{options.host, *port}};
#if !defined(_WIN32)
    options.required_peer_user = static_cast<weave::u64>(getuid()) + (mode == "local_wrong_user" ? 1u : 0u);
#endif
  }
  const bool native_gss = mode.starts_with("gss_");
  if (native_gss) {
    options.gss_encryption = pg::GssEncryption::require;
    if (mode == "gss_auth")
      options.gss_encryption = pg::GssEncryption::disable;
    else if (mode == "gss_prefer")
      options.gss_encryption = pg::GssEncryption::prefer;

    if (mode == "gss_wrong_service")
      options.gss_service = "absent_service";
    if (mode == "gss_deadline")
      options.connect_timeout = 20ms;

    pg::GssContextOptions provider{
      .workers = mode == "gss_reuse" ? 1u : 2u,
      .capacity = mode == "gss_reuse" ? 1u : 64u};
    if (mode != "gss_capture_default")
      provider.credential_cache = argv[10];

    auto gss = pg::GssContext::create(std::move(provider));
    check(gss.has_value());
    options.gss = *gss;

#if !defined(_WIN32)
    if (mode == "gss_capture_default")
      check(setenv("KRB5CCNAME", "FILE:/nonexistent/weave-health-cache", 1) == 0);
#endif
    if (mode == "gss_missing" || mode == "gss_wrong_service" || mode == "gss_deadline" || mode.ends_with("_cancel"))
      options.hosts.push_back({"localhost", *forbidden_port, *weave::IpAddress::parse("127.0.0.1")});
  }

  if (engine == "context") {
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    check(ctx->run(exercise(*ctx, options, mode)).has_value());
    check(ctx->metrics().submitted == ctx->metrics().completed);
  } else if (engine == "blocking") {
    if (mode == "unstarted") {
      auto pending = pg::ping(options);
    } else {
      unsigned repetitions = mode == "gss_reuse" ? 4 : 1;
      for (unsigned index = 0; index < repetitions; ++index)
        outcome(pg::ping_blocking(options), mode);
    }
  } else {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    auto runtime = weave::Runtime::create(
      {.workers = 4,
        .scheduler = engine.ends_with("stealing") ? weave::Scheduler::work_stealing : weave::Scheduler::worker_affine,
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
#if !defined(_WIN32)
  if (mode == "gss_auth") {
    using Entered = unsigned (*)();
    auto entered = reinterpret_cast<Entered>(dlsym(RTLD_DEFAULT, "weave_test_gss_entered"));
    check(entered != nullptr && entered() == 0);
  }
#endif
  std::printf("Live availability controls passed: %u checks; %s\n", checks.load(), OpenSSL_version(OPENSSL_VERSION));
}
