#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <source_location>
#include <openssl/crypto.h>

namespace pg = weave::pg;
static std::atomic<unsigned> checks = 0;
static std::mutex output_lock;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Metadata check failed at line %u\n", location.line());
    std::exit(1);
  }
}

static void hex(std::string_view value)
{
  for (auto character : value)
    std::printf("%02x", static_cast<unsigned>(static_cast<unsigned char>(character)));
}

static void optional(const std::optional<std::string> &value)
{
  if (!value) {
    std::printf("null");
    return;
  }
  std::printf("\"");
  hex(*value);
  std::printf("\"");
}

template <class C>
static pg::ConnectionInfo snapshot(C &connection, unsigned phase)
{
  auto info = connection.info();
  check(info.has_value());
  auto empty = connection.parameter("weave.fixture.empty");
  auto value = connection.parameter("weave.fixture.value");
  auto absent = connection.parameter("weave.fixture.missing");
  auto raw_version = connection.parameter("server_version");
  check(empty.has_value() && value.has_value() && absent.has_value() && raw_version.has_value());
  check(*empty && *value && !*absent);
  check(info->server_version == raw_version->value_or(""));
  auto invalid = connection.parameter(std::string_view{"bad\0name", 8});
  check(!invalid && invalid.error() == std::errc::invalid_argument && connection.open());
  {
    std::lock_guard lock(output_lock);
    std::printf("{\"phase\":%u,\"version\":%u,\"raw\":", phase, info->server_version_number);
    optional(*raw_version);
    std::printf(",\"options\":\"");
    hex(info->server_options);
    std::printf("\",\"empty\":");
    optional(*empty);
    std::printf(",\"value\":");
    optional(*value);
    std::printf(",\"absent\":");
    optional(*absent);
    std::printf("}\n");
  }
  return *info;
}

static weave::Task<void> session(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto retained = snapshot(connection, 0);
  auto old_value = connection.parameter("weave.fixture.value");
  check(old_value.has_value() && *old_value == "initial");
  {
    auto deferred = connection.query("NEVER");
    auto busy = connection.parameter("weave.fixture.value");
    check(!busy && busy.error() == pg::Error::busy);
  }
  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    auto busy = connection.parameter("weave.fixture.value");
    check(!busy && busy.error() == pg::Error::busy);
    check(pipeline->finish().has_value());
  }
  bool noticed = false;
  check(connection
      .on_notice([&](const pg::Diagnostic &) noexcept {
        auto busy = connection.parameter("weave.fixture.value");
        check(!busy && busy.error() == pg::Error::busy);
        noticed = true;
      })
      .has_value());
  co_await connection.query("NOOP");
  check(noticed && *old_value == "initial");
  auto changed = snapshot(connection, 1);
  check(changed.server_options == retained.server_options);
  auto moved = std::move(connection);
  auto missing = connection.parameter("weave.fixture.value");
  check(!missing && missing.error() == pg::Error::closed);
  options.server_options = "-c metadata.fixture=reset";
  co_await moved.reset(options);
  auto reset = snapshot(moved, 2);
  check(retained.server_options == "-c metadata.fixture=initial");
  check(reset.server_options == options.server_options);
  co_await moved.finish();
  auto closed = moved.parameter("weave.fixture.value");
  check(!closed && closed.error() == pg::Error::closed);
  check(retained.server_version_number == reset.server_version_number);
}

static void blocking(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection.has_value());
  auto retained = snapshot(*connection, 0);
  auto old_value = connection->parameter("weave.fixture.value");
  {
    auto pipeline = connection->pipeline();
    check(pipeline.has_value());
    auto busy = connection->parameter("weave.fixture.value");
    check(!busy && busy.error() == pg::Error::busy);
    check(pipeline->finish().has_value());
  }
  check(connection->query("NOOP").has_value());
  check(*old_value == "initial");
  snapshot(*connection, 1);
  auto moved = std::move(*connection);
  auto missing = connection->parameter("weave.fixture.value");
  check(!missing && missing.error() == pg::Error::closed);
  options.server_options = "-c metadata.fixture=reset";
  check(moved.reset(options).has_value());
  auto reset = snapshot(moved, 2);
  check(retained.server_options == "-c metadata.fixture=initial");
  check(reset.server_options == options.server_options);
  check(moved.finish().has_value());
  auto closed = moved.parameter("weave.fixture.value");
  check(!closed && closed.error() == pg::Error::closed);
  check(retained.server_version_number == reset.server_version_number);
}

int main(int argc, char **argv)
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  std::printf("OpenSSL: %s\n", OpenSSL_version(OPENSSL_VERSION));
  if (argc != 3)
    return 1;
  auto port = weave::parse_port(argv[1]);
  check(port.has_value());
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  options.server_options = "-c metadata.fixture=initial";
  std::string_view engine = argv[2];
  if (engine == "blocking") {
    blocking(options);
  } else if (engine == "context") {
    auto ctx = weave::Context::create();
    check(ctx.has_value());
    auto result = ctx->run(session(options));
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
      auto job = runtime->spawn(session(options));
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
  std::printf("Metadata controls passed: %u checks\n", checks.load());
}
