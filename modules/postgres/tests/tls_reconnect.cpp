#include <weave/postgres.hpp>
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include <weave/runtime.hpp>
#endif
#include "tls_certificates.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <iostream>

namespace pg = weave::pg;
static std::atomic<unsigned> checks{0};

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Refusal check failed: %s:%u\n", where.file_name(), where.line());
    std::exit(EXIT_FAILURE);
  }
}

static pg::Options options(weave::TlsContext tls, pg::GssContext gss, weave::u16 first, weave::u16 second)
{
  auto address = weave::IpAddress::parse("127.0.0.1");
  check(address.has_value());
  pg::Options result;
  result.user = "weave";
  result.hosts = {{"localhost", first, *address}, {"localhost", second, *address}};
  result.tls = std::move(tls);
  result.gss = std::move(gss);
  result.gss_encryption = pg::GssEncryption::prefer;
  result.tls_negotiation = pg::TlsNegotiation::direct;
  result.connect_timeout = std::chrono::seconds{3};
  return result;
}

static void history(const pg::ConnectionReport &report, weave::u16 first)
{
  check(report.completed && !report.truncated && report.attempts.size() == 1);
  const auto &attempt = report.attempts.front();
  check(attempt.host == "localhost" && attempt.port == first && attempt.endpoint.has_value());
  check(attempt.endpoint->port == first);
  check(attempt.stage == pg::ConnectionStage::tls);
  check(attempt.error == std::errc::connection_refused && report.error == attempt.error);
}

static weave::Task<void> failed(pg::Options settings, weave::u16 first)
{
  pg::ConnectionReport report;
  auto result = co_await weave::as_result(pg::connect(std::move(settings), report));
  if (result || result.error() != std::errc::connection_refused) {
    std::fprintf(
      stderr,
      "Unexpected result for %u: %s/%d\n",
      first,
      result ? "success" : result.error().category().name(),
      result ? 0 : result.error().value());
    for (const auto &attempt : report.attempts)
      std::fprintf(
        stderr,
        "Attempt %u stage %d: %s/%d\n",
        attempt.port,
        static_cast<int>(attempt.stage),
        attempt.error.category().name(),
        attempt.error.value());
  }
  check(!result && result.error() == std::errc::connection_refused);
  history(report, first);
}

int main()
{
  static fixture::Certificates files;
  auto tls = weave::TlsContext::client({.ca_file = files.ca});
  auto gss = pg::GssContext::create({.workers = 2, .capacity = 64});
  check(tls.has_value() && gss.has_value());
  auto ctx = weave::Context::create();
  check(ctx.has_value());
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
#ifdef _WIN32
  const std::array layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
#else
  const std::array layouts{weave::IoLayout::sharded};
#endif
  const auto count = 4 + schedulers.size() * layouts.size() * 8;
#else
  const auto count = std::size_t{4};
#endif
  std::printf("sessions %zu\n", count);
  std::fflush(stdout);
  std::vector<weave::u16> ports(count);
  unsigned second = 0;
  check(static_cast<bool>(std::cin >> second) && second > 0 && second <= 65535);
  for (auto &port : ports) {
    unsigned value = 0;
    check(static_cast<bool>(std::cin >> value) && value > 0 && value <= 65535);
    port = static_cast<weave::u16>(value);
  }
  auto next = std::size_t{0};
  auto settings = [&] {
    return options(*tls, *gss, ports[next++], static_cast<weave::u16>(second));
  };
  check(ctx->run(failed(settings(), ports.front())).has_value());
  auto ping = ctx->run(pg::ping(settings()));
  check(!ping && ping.error() == std::errc::connection_refused);
  pg::ConnectionReport report;
  auto blocking = pg::BlockingConnection::connect(settings(), report);
  check(!blocking && blocking.error() == std::errc::connection_refused);
  history(report, ports[2]);
  auto blocking_ping = pg::ping_blocking(settings());
  check(!blocking_ping && blocking_ping.error() == std::errc::connection_refused);

#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  for (auto scheduler : schedulers) {
    for (auto layout : layouts) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      check(runtime.has_value());
      std::vector<weave::JoinHandle<void>> jobs;
      for (int index = 0; index < 8; ++index) {
        auto first = ports[next];
        auto job = runtime->spawn(failed(settings(), first));
        check(job.has_value());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs)
        check(std::move(job).get().has_value());
    }
  }
#endif
  check(next == ports.size());
  std::printf("PASS refusal: %u checks\n", checks.load());
}
