#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
#include <atomic>
#include <cstdio>
#include <source_location>
#include <array>
#include <cstdlib>

namespace pg = weave::pg;
static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Direct TLS/GSS priority control failed: %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

static weave::Task<void> session(pg::Options options)
{
  auto connection = co_await pg::connect(options);

  auto info = connection.info();
  check(info.has_value() && info->gss_encrypted && !info->tls);
  check(info->authentication.method == pg::Authentication::gss && info->authentication.complete);

  auto rows = co_await connection.query("SELECT encrypted::text FROM pg_stat_gssapi WHERE pid=pg_backend_pid()");
  check(rows.front().rows.front().front().bytes() == "true");

  co_await connection.reset(options);
  info = connection.info();
  check(info.has_value() && info->gss_encrypted && !info->tls);
  rows = co_await connection.query("SELECT 42");
  check(rows.front().rows.front().front().bytes() == "42");

  co_await connection.finish();
}

int main(int argc, char **argv)
{
  check(argc == 3);
  auto port = weave::parse_port(argv[1]);
  auto gss = pg::GssContext::create({.workers = 2, .capacity = 64, .credential_cache = argv[2]});
  auto ctx = weave::Context::create();
  check(port.has_value() && gss.has_value() && ctx.has_value());
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);

  pg::Options options;
  options.host = "localhost";
  options.port = *port;
  options.user = "client";
  options.database = "postgres";
  options.tls_negotiation = pg::TlsNegotiation::direct;
  options.gss = *gss;
  options.authentication.methods = {pg::Authentication::gss};

  const std::array policies{pg::GssEncryption::prefer, pg::GssEncryption::require};
  for (auto policy : policies) {
    options.gss_encryption = policy;
    check(ctx->run(session(options)).has_value());

    auto blocking = pg::BlockingConnection::connect(options);
    check(blocking.has_value());
    auto info = blocking->info();
    check(info.has_value() && info->gss_encrypted && !info->tls);
    auto rows = blocking->query("SELECT encrypted::text FROM pg_stat_gssapi WHERE pid=pg_backend_pid()");
    check(rows.has_value() && rows->front().rows.front().front().bytes() == "true");
    check(blocking->reset(options).has_value());
    info = blocking->info();
    check(info.has_value() && info->gss_encrypted && !info->tls);
    check(blocking->finish().has_value());

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
      check(runtime.has_value());

      std::vector<weave::JoinHandle<void>> jobs;
      for (int i = 0; i < 8; ++i) {
        auto job = runtime->spawn(session(options));
        check(job.has_value());
        jobs.push_back(std::move(*job));
      }

      for (auto &job : jobs)
        check(std::move(job).get().has_value());
    }
#endif
  }

  std::printf("Direct TLS/GSS priority: %u checks passed\n", checks.load());
}
