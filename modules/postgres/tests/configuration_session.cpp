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
using namespace std::chrono_literals;
static std::atomic<unsigned> checks = 0;
static std::mutex output_lock;

static void check(bool value, std::source_location where = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Configuration session check failed: %u\n", where.line());
    std::_Exit(1);
  }
}

template <class C>
static void busy(C &connection)
{
  auto rejected = connection.configuration();
  check(!rejected && rejected.error() == pg::Error::busy);
  check(connection.open());
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
static pg::OptionsInfo snapshot(C &connection, const pg::Options &options, unsigned phase)
{
  auto configuration = connection.configuration();
  check(configuration.has_value());
  check(configuration->host == options.host && configuration->port == options.port);
  check(configuration->host == "configured-unused.invalid");
  check(configuration->hosts.size() == 1 && configuration->hosts[0].name == "127.0.0.1");
  check(configuration->hosts[0].address == options.hosts[0].address);
  check(configuration->hosts[0].port == options.hosts[0].port);
  check(configuration->user == options.user && configuration->database == options.user);
  check(configuration->application_name == options.application_name);
  check(configuration->server_options == options.server_options && configuration->plaintext);
  check(configuration->client_encoding == "UTF8" && !configuration->tls_options);
  check(configuration->connect_timeout == 23456ms && configuration->settings == options.settings);
  check(configuration->origin && configuration->origin->service == "historical-source");
  check(!configuration->tls_context && !configuration->gss_context && !configuration->oauth);

  auto actual = connection.info();
  check(actual.has_value());
  check(actual->host == "127.0.0.1" && actual->port == options.hosts[0].port);
  check(actual->server_options == options.server_options && actual->database == options.user);
  auto empty = connection.parameter("weave.fixture.empty");
  auto value = connection.parameter("weave.fixture.value");
  auto absent = connection.parameter("weave.fixture.missing");
  auto version = connection.parameter("server_version");
  check(empty.has_value() && value.has_value() && absent.has_value() && version.has_value());
  check(*empty && *value && !*absent);
  auto last = connection.last_failure();
  check(last.has_value() && !last->error);
  {
    std::lock_guard lock(output_lock);
    std::printf("{\"phase\":%u,\"version\":%u,\"raw\":", phase, actual->server_version_number);
    optional(*version);
    std::printf(",\"options\":\"");
    hex(actual->server_options);
    std::printf("\",\"empty\":");
    optional(*empty);
    std::printf(",\"value\":");
    optional(*value);
    std::printf(",\"absent\":");
    optional(*absent);
    std::printf("}\n");
  }
  return *configuration;
}

static weave::Task<void> session(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto retained = snapshot(connection, options, 0);
  retained.hosts[0].name = "copy-mutated";
  check(connection.configuration()->hosts[0].name == "127.0.0.1");
  {
    auto deferred = connection.query("NEVER");
    busy(connection);
    auto next = options;
    next.application_name = "must-not-install";
    auto reset = co_await weave::as_result(connection.reset(std::move(next)));
    check(!reset && reset.error() == pg::Error::busy);
  }
  check(connection.configuration()->application_name == options.application_name);
  {
    auto deferred = connection.reset(options);
    busy(connection);
  }
  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    busy(connection);
    check(pipeline->finish().has_value());
  }
  bool noticed = false;
  check(connection
      .on_notice([&](const pg::Diagnostic &) noexcept {
        busy(connection);
        noticed = true;
      })
      .has_value());
  unsigned traces = 0;
  check(connection
      .on_trace(
        {.handler =
            [&](const pg::TraceMessage &) noexcept {
              busy(connection);
              ++traces;
            }})
      .has_value());
  unsigned events = 0;
  auto registration = connection.on_event("configuration", [&](pg::Event &event) noexcept -> weave::Result<void> {
    if (event.connection && event.kind != pg::EventKind::connection_destroy) {
      busy(*event.connection);
      ++events;
    }
    return {};
  });
  check(registration.has_value());

  co_await connection.query("NOOP");
  check(noticed && traces > 0 && events > 1);
  snapshot(connection, options, 1);
  auto invalid = options;
  invalid.connect_timeout = 0ms;
  auto rejected = co_await weave::as_result(connection.reset(std::move(invalid)));
  check(!rejected && rejected.error() == std::errc::invalid_argument && connection.open());
  check(connection.configuration()->application_name == options.application_name);
  check(connection.on_notice({}).has_value() && connection.on_trace({}).has_value());

  auto moved = std::move(connection);
  auto missing = connection.configuration();
  check(!missing && missing.error() == pg::Error::closed);
  options.application_name = "after-reset";
  options.server_options = "-c metadata.fixture=reset";
  co_await moved.reset(options);
  auto reset = snapshot(moved, options, 2);
  check(reset.application_name == "after-reset" && retained.application_name == "before-reset");
  check(retained.hosts[0].name == "copy-mutated");
  co_await moved.finish();
  check(!moved.open());
  auto closed = moved.configuration();
  check(closed.has_value() && closed->application_name == "after-reset");
  check(closed->hosts[0].name == "127.0.0.1");
}

static void blocking(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection.has_value());
  auto retained = snapshot(*connection, options, 0);
  {
    auto pipeline = connection->pipeline();
    check(pipeline.has_value());
    busy(*connection);
    check(pipeline->finish().has_value());
  }
  bool noticed = false;
  check(connection
      ->on_notice([&](const pg::Diagnostic &) noexcept {
        busy(*connection);
        noticed = true;
      })
      .has_value());
  check(connection->query("NOOP").has_value() && noticed);
  snapshot(*connection, options, 1);
  auto invalid = options;
  invalid.connect_timeout = 0ms;
  auto rejected = connection->reset(std::move(invalid));
  check(!rejected && rejected.error() == std::errc::invalid_argument && connection->open());
  check(connection->on_notice({}).has_value());
  auto moved = std::move(*connection);
  auto missing = connection->configuration();
  check(!missing && missing.error() == pg::Error::closed);
  options.application_name = "after-reset";
  options.server_options = "-c metadata.fixture=reset";
  check(moved.reset(options).has_value());
  auto reset = snapshot(moved, options, 2);
  check(reset.application_name == "after-reset" && retained.application_name == "before-reset");
  check(moved.finish().has_value());
  auto closed = moved.configuration();
  check(closed.has_value() && closed->server_options == options.server_options);
}

int main(int argc, char **argv)
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  std::printf("OpenSSL: %s\n", OpenSSL_version(OPENSSL_VERSION));
  if (argc != 3)
    return 1;
  auto port = weave::parse_port(argv[1]);
  auto address = weave::IpAddress::parse("127.0.0.1");
  check(port.has_value() && address.has_value());
  pg::Options options{.host = "configured-unused.invalid", .port = 6543, .user = "test", .plaintext = true};
  options.application_name = "before-reset";
  options.server_options = "-c metadata.fixture=initial";
  options.connect_timeout = 23456ms;
  options.settings = {{"weave.fixture.startup", "one"}};
  options.hosts = {{"127.0.0.1", *port, *address, std::string(127, 'h')}};
  options.password.assign(137, 'p');
  options.origin = pg::ConfigurationOrigin{.service = "historical-source"};
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
