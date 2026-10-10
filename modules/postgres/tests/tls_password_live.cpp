#include "tls_password_support.hpp"
#include <iostream>

static weave::pg::Options live_options(
  fixture::Certificates &files,
  const std::string &key,
  weave::u16 port,
  const std::string &password,
  const weave::IpAddress &address,
  const std::shared_ptr<int> &lifetime)
{
  weave::pg::Options options;
  options.user = "weave";
  options.database = "postgres";
  options.port = port;
  options.password = password;
  options.host = "localhost";
  options.hosts = {{"localhost", port, address}};
  options.channel_binding = weave::pg::ChannelBinding::require;
  options.tls_options = client_options(files, key);
  auto provider = weave::TlsPasswordProvider::create(
    [lifetime](std::string_view) noexcept -> weave::Result<std::string> {
      return std::string(127, 'p');
    });
  check(provider.has_value());
  options.tls_options->private_key_password_provider = std::move(*provider);
  return options;
}

static weave::Task<void> live_async(weave::pg::Options options, std::weak_ptr<int> weak)
{
  auto connection = co_await weave::pg::connect(std::move(options));
  check(weak.expired());
  auto info = connection.info();
  check(info.has_value() && info->tls && info->authentication.complete);
  check(info->authentication.method == weave::pg::Authentication::scram_sha256);
  auto configuration = connection.configuration();
  check(configuration.has_value() && configuration->tls_options->private_key_password_provider);
  auto result = co_await connection.query("SELECT 71");
  check(result.size() == 1 && result.front().rows.front().front().bytes() == "71");
  co_await connection.finish();
}

static weave::Task<void> reset_failure(
  weave::pg::Options good,
  weave::pg::Options bad,
  std::error_code expected,
  const std::atomic<int> &requests)
{
  auto connection = co_await weave::pg::connect(std::move(good));
  weave::pg::ConnectionReport report;
  auto result = co_await weave::as_result(connection.reset(std::move(bad), report));
  check(!result && result.error() == expected);
  check(report.completed && report.attempts.size() == 1);
  check(report.attempts.front().error == expected && requests == 1);
  check(!connection.open());
}

static void reset_failures(
  fixture::Certificates &files,
  const std::string &key,
  weave::u16 port,
  weave::u16 standby,
  const std::string &password,
  const weave::IpAddress &address)
{
  auto good = [&] {
    return live_options(files, key, port, password, address, std::make_shared<int>(7));
  };
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  const std::array errors{
    std::error_code{},
    std::make_error_code(std::errc::timed_out),
    std::make_error_code(std::errc::connection_reset),
    weave::pg::make_error_code(weave::pg::Error::target_session)};
  for (auto error : errors) {
    std::atomic<int> requests{0};
    auto bad = [&] {
      auto options = good();
      options.hosts.push_back({"localhost", standby, address});
      auto provider = weave::TlsPasswordProvider::create(
        [error, &requests](std::string_view) noexcept -> weave::Result<std::string> {
          ++requests;
          return std::unexpected(error);
        });
      check(provider.has_value());
      options.tls_options->private_key_password_provider = std::move(*provider);
      return options;
    };
    auto connection = weave::pg::BlockingConnection::connect(good());
    check(connection.has_value());
    weave::pg::ConnectionReport report;
    auto result = connection->reset(bad(), report);
    check(!result && result.error() == error);
    check(report.completed && report.attempts.size() == 1);
    check(report.attempts.front().error == error && requests == 1);
    check(!connection->open());
    requests = 0;
    check(ctx->run(reset_failure(good(), bad(), error, requests)).has_value());
  }
}

static weave::Task<void> reset_ownership(
  weave::pg::Options good,
  fixture::Certificates &files,
  const std::string &key,
  weave::u16 port,
  const std::string &password,
  const weave::IpAddress &address)
{
  auto connection = co_await weave::pg::connect(std::move(good));
  auto lifetime = std::make_shared<int>(9);
  std::weak_ptr<int> weak = lifetime;
  auto options = live_options(files, key, port, password, address, lifetime);
  lifetime.reset();
  {
    auto unstarted = connection.reset(std::move(options));
    check(!weak.expired());
  }
  check(weak.expired());
  auto result = co_await connection.query("SELECT 74");
  check(result.front().rows.front().front().bytes() == "74");

  auto rejecting = weave::Context::create();
  check(rejecting.has_value());
  rejecting->request_stop();
  lifetime = std::make_shared<int>(10);
  weak = lifetime;
  options = live_options(files, key, port, password, address, lifetime);
  lifetime.reset();
  check(!rejecting->spawn(connection.reset(std::move(options))));
  check(weak.expired());
  result = co_await connection.query("SELECT 75");
  check(result.front().rows.front().front().bytes() == "75");

  lifetime = std::make_shared<int>(11);
  weak = lifetime;
  options = live_options(files, key, port, password, address, lifetime);
  lifetime.reset();
  co_await connection.reset(std::move(options));
  check(weak.expired());
  result = co_await connection.query("SELECT 76");
  check(result.front().rows.front().front().bytes() == "76");
  co_await connection.finish();
}

int main()
{
  fixture::EncryptedCertificates encrypted{std::string(127, 'p')};
  auto &files = encrypted.certificates;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string text, password, standby, address_text, local;
  check(static_cast<bool>(std::getline(std::cin, text)));
  check(static_cast<bool>(std::getline(std::cin, password)));
  check(static_cast<bool>(std::getline(std::cin, standby)));
  check(static_cast<bool>(std::getline(std::cin, address_text)));
  check(static_cast<bool>(std::getline(std::cin, local)));
  auto port = weave::parse_port(text);
  auto address = weave::IpAddress::parse(address_text);
  check(port.has_value() && address.has_value());
  const auto &key = encrypted.client_key;

  auto lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> weak = lifetime;
  auto options = live_options(files, key, *port, password, *address, lifetime);
  lifetime.reset();
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  check(ctx->run(live_async(std::move(options), weak)).has_value());

  lifetime = std::make_shared<int>(2);
  weak = lifetime;
  options = live_options(files, key, *port, password, *address, lifetime);
  lifetime.reset();
  auto blocking = weave::pg::BlockingConnection::connect(std::move(options));
  check(blocking.has_value() && weak.expired());
  auto result = blocking->query("SELECT 72");
  check(result.has_value() && result->front().rows.front().front().bytes() == "72");
  lifetime = std::make_shared<int>(3);
  weak = lifetime;
  options = live_options(files, key, *port, password, *address, lifetime);
  lifetime.reset();
  check(blocking->reset(std::move(options)).has_value());
  check(weak.expired());
  check(blocking->finish().has_value());

#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto layout : support::io_layouts) {
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      check(runtime.has_value());
      std::vector<weave::JoinHandle<void>> jobs;
      for (int i = 0; i < 16; ++i) {
        lifetime = std::make_shared<int>(4);
        weak = lifetime;
        options = live_options(files, key, *port, password, *address, lifetime);
        lifetime.reset();
        auto job = runtime->spawn(live_async(std::move(options), weak));
        check(job.has_value());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs)
        check(std::move(job).get().has_value());
    }
  }
#endif
  auto standby_port = weave::parse_port(standby);
  check(standby_port.has_value());
  reset_failures(files, key, *port, *standby_port, password, *address);
  options = live_options(files, key, *port, password, *address, std::make_shared<int>(12));
  check(ctx->run(reset_ownership(std::move(options), files, key, *port, password, *address)).has_value());
  std::printf("Live provider: %u checks passed\n", checks.load());
}
