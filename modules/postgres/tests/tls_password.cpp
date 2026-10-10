#include "tls_password_support.hpp"

static weave::pg::Options pg_options(
  fixture::Certificates &files,
  const std::string &key,
  weave::u16 first,
  weave::u16 second,
  std::error_code error,
  std::atomic<int> &calls)
{
  weave::pg::Options options;
  options.user = "weave";
  auto address = weave::IpAddress::parse("127.0.0.1");
  check(address.has_value());
  options.hosts = {{"localhost", first, *address}, {"localhost", second, *address}};
  options.tls_options = client_options(files, key);
  auto provider = weave::TlsPasswordProvider::create(
    [error, &calls](std::string_view) noexcept -> weave::Result<std::string> {
      ++calls;
      return std::unexpected(error);
    });
  check(provider.has_value());
  options.tls_options->private_key_password_provider = std::move(*provider);
  return options;
}

static weave::Task<void> connection_failure(weave::pg::Options options, std::error_code expected)
{
  auto result = co_await weave::as_result(weave::pg::connect(std::move(options)));
  check(!result && result.error() == expected);
}

static void postgres(fixture::Certificates &files, const std::string &key, weave::u16 first, weave::u16 second)
{
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  std::atomic<int> calls{0};
  const std::array errors{
    std::error_code{},
    std::make_error_code(std::errc::timed_out),
    std::make_error_code(std::errc::connection_reset),
    weave::pg::make_error_code(weave::pg::Error::target_session)};
  for (auto error : errors) {
    auto options = pg_options(files, key, first, second, error, calls);
    auto info = options.info();
    check(info.tls_options->private_key_password_provider);
    check(ctx->run(connection_failure(options, error)).has_value());
    auto blocking = weave::pg::BlockingConnection::connect(options);
    check(!blocking && blocking.error() == error);
    auto ping = ctx->run(weave::pg::ping(options));
    check(!ping && ping.error() == error);
    weave::pg::ConnectionReport report;
    auto reported = ctx->run(weave::pg::connect(options, report));
    check(!reported && reported.error() == error);
    check(report.completed && report.attempts.size() == 1);
    check(report.attempts.front().error == error);
  }
  check(calls == 16);

  const auto error = std::make_error_code(std::errc::timed_out);
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  auto runtime = weave::Runtime::create({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
  check(runtime.has_value());
  std::vector<weave::JoinHandle<void>> jobs;
  for (int i = 0; i < 24; ++i) {
    auto job = runtime->spawn(connection_failure(pg_options(files, key, first, second, error, calls), error));
    check(job.has_value());
    jobs.push_back(std::move(*job));
  }
  for (auto &job : jobs)
    check(std::move(job).get().has_value());
  check(calls == 40);
#endif

  auto lifetime = std::make_shared<int>(1);
  std::weak_ptr<int> weak = lifetime;
  auto provider = weave::TlsPasswordProvider::create(
    [lifetime](std::string_view) noexcept -> weave::Result<std::string> {
      return std::string(127, 'p');
    });
  auto options = pg_options(files, key, first, second, error, calls);
  options.tls_options->private_key_password_provider = std::move(*provider);
  lifetime.reset();
  {
    auto unstarted = weave::pg::connect(std::move(options));
    check(!weak.expired());
  }
  check(weak.expired());

  auto rejecting = weave::Context::create();
  check(rejecting.has_value());
  rejecting->request_stop();
  lifetime = std::make_shared<int>(2);
  weak = lifetime;
  provider = weave::TlsPasswordProvider::create([lifetime](std::string_view) noexcept -> weave::Result<std::string> {
    return std::string(127, 'p');
  });
  options = pg_options(files, key, first, second, error, calls);
  options.tls_options->private_key_password_provider = std::move(*provider);
  lifetime.reset();
  check(!rejecting->spawn(weave::pg::connect(std::move(options))));
  check(weak.expired());
}

int main(int argc, char **argv)
{
  check(argc == 3);
  auto first = weave::parse_port(argv[1]);
  auto second = weave::parse_port(argv[2]);
  check(first.has_value() && second.has_value());

  std::filesystem::path directory;
  {
    fixture::EncryptedCertificates files{std::string(127, 'p')};
    directory = files.certificates.directory;
    std::printf("Owned certificate fixture: %s\n", directory.string().c_str());
    std::fflush(stdout);
    postgres(files.certificates, files.client_key, *first, *second);
  }
  check(!std::filesystem::exists(directory));
  std::printf("PostgreSQL key provider: %u checks passed\n", checks.load());
}
