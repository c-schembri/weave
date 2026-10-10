#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <openssl/crypto.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <source_location>

namespace pg = weave::pg;
static std::atomic<unsigned> checks = 0;
static std::mutex output;

static void check(bool condition, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Authentication metadata check failed at %u\n", where.line());
    std::exit(1);
  }
}

static bool same(const pg::AuthenticationInfo &a, const pg::AuthenticationInfo &b)
{
  return a.method == b.method && a.password_requested == b.password_requested &&
    a.password_missing == b.password_missing && a.complete == b.complete;
}

static void emit(const pg::ConnectionReport &report, bool success)
{
  check(report.completed && success == !report.error);
  if (success)
    check(report.attempts.empty());
  else {
    check(report.attempts.size() == 1);
    check(same(report.authentication, report.attempts.front().authentication));
  }
  auto copy = report;
  check(same(copy.authentication, report.authentication));
  auto text = copy.format();
  check(text.has_value());
  check(text->find("secret-source") == std::string::npos);
  {
    std::lock_guard lock(output);
    const auto &auth = report.authentication;
    std::printf(
      "{\"success\":%s,\"method\":%u,\"requested\":%s,\"missing\":%s,\"complete\":%s}\n",
      success ? "true" : "false",
      static_cast<unsigned>(auth.method),
      auth.password_requested ? "true" : "false",
      auth.password_missing ? "true" : "false",
      auth.complete ? "true" : "false");
  }
}

static weave::Task<void> session(weave::Context &ctx, pg::Options options, std::string_view scenario)
{
  pg::ConnectionReport report;
  report.authentication = {.method = pg::Authentication::oauth, .complete = true};
  {
    auto deferred = pg::connect(options, report);
    check(report.authentication.method == pg::Authentication::oauth && report.authentication.complete);
  }
  check(report.authentication.method == pg::Authentication::oauth && report.authentication.complete);
  weave::CancelSource cancellation;
  auto operation = pg::connect(options, report, [&](const pg::Diagnostic &notice) noexcept {
    if (notice.message() == "CANCEL")
      cancellation.cancel();
  });
  std::optional<weave::JoinHandle<pg::Connection>> job;
  if (scenario == "scram_cancel") {
    auto submitted = ctx.spawn(std::move(operation), {.cancel = cancellation.token()});
    check(submitted.has_value());
    job.emplace(std::move(*submitted));
  }
  auto result = job ? co_await weave::as_result(std::move(*job)) : co_await weave::as_result(std::move(operation));
  if (scenario == "scram_cancel")
    check(!result && result.error() == std::errc::operation_canceled);
  if (scenario.starts_with("reset")) {
    check(result.has_value());
    auto old = result->info();
    check(old && old->authentication.complete && !old->authentication.password_requested);
    {
      auto deferred = result->query("NEVER");
      pg::ConnectionReport rejected;
      rejected.authentication = {.method = pg::Authentication::oauth, .complete = true};
      auto busy = co_await weave::as_result(result->reset(options, rejected));
      check(!busy && busy.error() == pg::Error::busy && result->open());
      check(!rejected.authentication.complete && !rejected.authentication.password_requested);
      check(rejected.authentication.method == pg::Authentication::none);
    }
    options.application_name = "auth-info-reset";
    auto reset = co_await weave::as_result(result->reset(options, report));
    check(old->authentication.complete && !old->authentication.password_requested);
    if (reset) {
      auto info = result->info();
      check(info && same(info->authentication, report.authentication));
      co_await result->finish();
    } else {
      check(!result->open());
    }
    emit(report, reset.has_value());
    co_return;
  }
  if (result) {
    auto info = result->info();
    check(info.has_value());
    check(same(info->authentication, report.authentication));
    auto moved = std::move(*result);
    co_await moved.finish();
    check(same(info->authentication, report.authentication));
  }
  emit(report, result.has_value());
}

static void blocking(pg::Options options, std::string_view scenario)
{
  pg::ConnectionReport report;
  auto result = pg::BlockingConnection::connect(options, report);
  if (scenario.starts_with("reset")) {
    check(result.has_value());
    auto old = result->info();
    check(old && old->authentication.complete && !old->authentication.password_requested);
    {
      auto pipeline = result->pipeline();
      check(pipeline.has_value());
      pg::ConnectionReport rejected;
      rejected.authentication = {.method = pg::Authentication::oauth, .complete = true};
      auto busy = result->reset(options, rejected);
      check(!busy && busy.error() == pg::Error::busy && result->open());
      check(!rejected.authentication.complete && !rejected.authentication.password_requested);
      check(rejected.authentication.method == pg::Authentication::none);
      check(pipeline->finish().has_value());
    }
    options.application_name = "auth-info-reset";
    auto reset = result->reset(options, report);
    check(old->authentication.complete && !old->authentication.password_requested);
    if (reset) {
      auto info = result->info();
      check(info && same(info->authentication, report.authentication));
      check(result->finish().has_value());
    } else {
      check(!result->open());
    }
    emit(report, reset.has_value());
    return;
  }
  if (result) {
    auto info = result->info();
    check(info.has_value());
    check(same(info->authentication, report.authentication));
    auto moved = std::move(*result);
    check(moved.finish().has_value());
    check(same(info->authentication, report.authentication));
  }
  emit(report, result.has_value());
}

int main(int argc, char **argv)
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  std::printf("OpenSSL: %s\n", OpenSSL_version(OPENSSL_VERSION));
  if (argc != 4)
    return 1;

  auto port = weave::parse_port(argv[1]);
  check(port.has_value());

  std::string_view engine = argv[2];
  std::string_view scenario = argv[3];
  const bool missing_password = scenario.ends_with("missing") || scenario == "scram_keys" || scenario == "reset_keys";
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  options.password = missing_password ? "" : "secret-source";
  options.application_name = "auth-info-initial";
  options.allow_md5_password = true;
  options.channel_binding = pg::ChannelBinding::disable;

  if (scenario == "scram_keys" || scenario == "reset_keys") {
    auto client = pg::ScramKey::parse("Tcn749j882CnenZTl3ZR7IGG65V2ZkUuWRy7HLaHwm0=");
    auto server = pg::ScramKey::parse("cKMH7kFpT91Ant4x4cVouJ9kma3Q5QACQagodKK123Y=");
    check(client.has_value() && server.has_value());
    options.scram_client_key = std::move(*client);
    options.scram_server_key = std::move(*server);
  }

  if (engine == "blocking") {
    blocking(options, scenario);
  } else if (engine == "context") {
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    auto result = ctx->run(session(*ctx, options, scenario));
    if (!result)
      return weave::report_error(result.error());
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  else {
    weave::RuntimeOptions configuration{
      .workers = 4,
      .scheduler = engine.ends_with("affine") ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing,
      .io_layout = engine.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded};
    std::printf(
      "Runtime: workers=4 scheduler=%s io=%s roots=16\n",
      configuration.scheduler == weave::Scheduler::worker_affine ? "affine" : "stealing",
      configuration.io_layout == weave::IoLayout::shared ? "shared" : "sharded");
    auto runtime = weave::Runtime::create(configuration);
    check(runtime.has_value());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned root = 0; root < 16; ++root) {
      auto job = runtime->spawn([options, scenario](weave::Context &ctx) {
        return session(ctx, options, scenario);
      });
      check(job.has_value());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#else
  else {
    return 1;
  }
#endif

  std::printf("Authentication metadata passed: %u checks\n", checks.load());
}
