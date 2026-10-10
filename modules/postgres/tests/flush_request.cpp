#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/scope.hpp>
#if defined(WEAVE_POSTGRES_TEST_LIVE)
#include <weave/timer.hpp>
#include "tls_certificates.hpp"
#include <iostream>
#endif
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace pg = weave::pg;
static std::atomic<unsigned> checks = 0;

template <class T>
static void check(const T &value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Flush request check failed: %u\n", location.line());
    std::exit(1);
  }
}

template <class C>
static void idle(C &connection)
{
  check(connection.open() && connection.transaction() == pg::Transaction::idle);
}

static weave::Task<void> session(pg::Options options, bool live = false)
{
  const char *noop = live ? "DO $$ BEGIN END $$" : "NOOP";
  const char *error = live ? "SELECT 1 / 0" : "ERROR";
  auto connection = co_await pg::connect(options);
  std::string sent;
  check(connection.on_trace({.handler = [&](const pg::TraceMessage &message) noexcept {
    if (message.direction == pg::TraceDirection::frontend)
      sent += message.kind;
  }}));

  {
    auto pipeline = connection.pipeline();
    check(pipeline);
    check(pipeline->request_flush());
    check(sent.empty());
    idle(connection);
    check(!pipeline->finish() && !pipeline->try_next()->has_value());
    {
      auto deferred = pipeline->flush();
      check(!pipeline->finish());
    }
    auto moved = std::move(*pipeline);
    co_await moved.flush();
    check(sent == "HH");
    check(!(co_await moved.next()));
    check(moved.finish());
    auto closed = moved.request_flush();
    check(!closed && closed.error() == pg::Error::closed);
  }
  idle(connection);
  sent.clear();

  {
    auto unsent = connection.pipeline();
    check(unsent && unsent->request_flush());
  }
  check(sent.empty());
  idle(connection);

  {
    auto pipeline = connection.pipeline();
    check(pipeline && pipeline->request_flush());
    co_await pipeline->receive();
    check(sent.empty() && !pipeline->finish());
    co_await pipeline->send();
    check(sent == "HH" && !(co_await pipeline->next()));
    check(pipeline->finish());
  }
  sent.clear();
  idle(connection);

  {
    auto pipeline = connection.pipeline();
    check(pipeline);
    auto first = pipeline->execute({noop});
    check(first && *first == 1 && pipeline->request_flush());
    auto second = pipeline->execute({noop});
    auto barrier = pipeline->sync();
    check(second && *second == 2 && barrier && *barrier == 3 && pipeline->request_flush());
    check(sent.empty());
    co_await weave::when_all(pipeline->receive(), pipeline->send());
    check(sent == "PBDEHPBDESHH");
    const std::array ids{*first, *second, *barrier};
    for (auto id : ids)
      check((co_await pipeline->next())->id == id);
    check(!(co_await pipeline->next()) && pipeline->finish());
  }
  sent.clear();
  idle(connection);

  {
    auto pipeline = connection.pipeline();
    check(pipeline && pipeline->execute({error}) && pipeline->request_flush());
    check(pipeline->execute({noop}));
    co_await pipeline->flush();
    check(pipeline->aborted());
    check((co_await pipeline->next())->outcome.error.sqlstate() == "22012");
    check((co_await pipeline->next())->outcome.aborted);
    check(!(co_await pipeline->next()));
    check(pipeline->request_flush());
    co_await pipeline->send();
    check(pipeline->aborted() && !pipeline->finish());
    check(pipeline->sync());
    co_await pipeline->flush();
    check(!pipeline->aborted() && (co_await pipeline->next())->kind == pg::PipelineKind::sync);
    check(!(co_await pipeline->next()) && pipeline->finish());
    check(sent == "PBDEHPBDEHHHSH");
  }
  idle(connection);
  check(connection.on_trace({}));
  co_await connection.finish();

  if (live)
    co_return;

  options.limits.message_bytes = 1024;
  options.limits.result_bytes = 1024;
  options.limits.pipeline_commands = 1;
  auto limited = co_await pg::connect(options);
  {
    auto pipeline = limited.pipeline();
    check(pipeline);
    constexpr unsigned accepted = 1024 / 5;
    for (unsigned index = 0; index < accepted; ++index)
      check(pipeline->request_flush());
    auto denied = pipeline->request_flush();
    check(!denied && denied.error() == pg::Error::resource_limit);
    co_await pipeline->flush();
    check(pipeline->finish());
  }
  {
    auto pipeline = limited.pipeline();
    check(pipeline && pipeline->execute({"NOOP"}) && pipeline->request_flush());
    auto denied = pipeline->execute({"NOOP"});
    check(!denied && denied.error() == pg::Error::resource_limit);
    co_await pipeline->flush();
    check((co_await pipeline->next())->id == 1);
    auto barrier = pipeline->sync();
    check(barrier && *barrier == 2);
    co_await pipeline->flush();
    check((co_await pipeline->next())->id == 2 && !(co_await pipeline->next()));
    check(pipeline->finish());
  }
  idle(limited);
  co_await limited.finish();
}

static void blocking(pg::Options options, bool live = false)
{
  const char *noop = live ? "DO $$ BEGIN END $$" : "NOOP";
  auto connection = pg::BlockingConnection::connect(options);
  check(connection);
  {
    auto pipeline = connection->pipeline();
    check(pipeline && pipeline->request_flush());
    check(!pipeline->finish() && !pipeline->next()->has_value());
    check(pipeline->flush());
    check(pipeline->finish());
    auto closed = pipeline->request_flush();
    check(!closed && closed.error() == pg::Error::closed);
  }
  idle(*connection);
  {
    auto pipeline = connection->pipeline();
    check(pipeline && pipeline->request_flush());
    check(pipeline->send());
    auto denied = pipeline->request_flush();
    check(!denied && denied.error() == pg::Error::busy);
    check(pipeline->receive());
    check(pipeline->finish());
  }
  {
    auto pipeline = connection->pipeline();
    check(pipeline && pipeline->execute({noop}) && pipeline->request_flush());
    check(pipeline->sync() && pipeline->flush());
    check(pipeline->next()->has_value());
    check(pipeline->next()->has_value());
    check(!pipeline->next()->has_value() && pipeline->finish());
  }
  idle(*connection);
  check(connection->finish());
}

static weave::Task<void> terminal(pg::Options options, std::string_view scenario, bool live = false)
{
  auto connection = co_await pg::connect(options);
  weave::TaskScope tasks;
  if (scenario == "cancel_after") {
    check(connection.on_notice([&](const pg::Diagnostic &notice) noexcept {
      check(notice.message() == "PENDING");
      tasks.cancel();
    }));
  }
  auto pipeline = connection.pipeline();
  check(pipeline);

  if (scenario == "cancel_before") {
    check(pipeline->request_flush());
    tasks.cancel();
    auto job = tasks.spawn(pipeline->send());
    check(job);
    auto cancelled = co_await weave::as_result(std::move(*job));
    check(!cancelled && cancelled.error() == std::errc::operation_canceled);
    auto drained = co_await weave::as_result(tasks.join());
    check(!drained && drained.error() == std::errc::operation_canceled);
    check(!pipeline->finish());
    idle(connection);
    co_await pipeline->flush();
    check(pipeline->finish());
    co_await connection.finish();
    co_return;
  }

  std::error_code expected;
  if (scenario == "cancel_after") {
    const char *wait = live ? "DO $$ BEGIN RAISE NOTICE 'PENDING'; PERFORM pg_sleep(30); END $$" : "WAIT";
    check(pipeline->execute({wait}) && pipeline->request_flush());
    auto job = tasks.spawn(pipeline->flush());
    check(job);
    auto cancelled = co_await weave::as_result(std::move(*job));
    check(!cancelled && cancelled.error() == std::errc::operation_canceled);
    auto drained = co_await weave::as_result(tasks.join());
    check(!drained && drained.error() == std::errc::operation_canceled);
    expected = std::make_error_code(std::errc::operation_canceled);
  } else {
    check(!live && (scenario == "eof" || scenario == "bad"));
    check(pipeline->execute({scenario == "eof" ? "EOF" : "BAD"}) && pipeline->request_flush());
    auto failed = co_await weave::as_result(pipeline->flush());
    if (!failed)
      std::printf(
        "Terminal Flush failure: scenario=%.*s code=%d category=%s\n",
        static_cast<int>(scenario.size()),
        scenario.data(),
        failed.error().value(),
        failed.error().category().name());
    expected = scenario == "eof" ? std::make_error_code(std::errc::connection_reset)
                                 : pg::make_error_code(pg::Error::protocol);
    check(!failed && failed.error() == expected);
  }

  check(!connection.open() && connection.transaction() == pg::Transaction::unknown);
  auto rejected = pipeline->request_flush();
  check(!rejected && rejected.error() == expected);
  auto finished = pipeline->finish();
  check(!finished && finished.error() == expected);
}

static void blocking_terminal(pg::Options options, std::string_view scenario)
{
  check(scenario == "eof" || scenario == "bad");
  auto connection = pg::BlockingConnection::connect(options);
  check(connection);
  auto pipeline = connection->pipeline();
  check(pipeline && pipeline->execute({scenario == "eof" ? "EOF" : "BAD"}) && pipeline->request_flush());
  auto failed = pipeline->flush();
  if (!failed)
    std::printf(
      "Terminal Flush failure: scenario=%.*s code=%d category=%s\n",
      static_cast<int>(scenario.size()),
      scenario.data(),
      failed.error().value(),
      failed.error().category().name());
  auto expected = scenario == "eof" ? std::make_error_code(std::errc::connection_reset)
                                    : pg::make_error_code(pg::Error::protocol);
  check(!failed && failed.error() == expected && !connection->open());
  auto rejected = pipeline->request_flush();
  check(!rejected && rejected.error() == expected);
}

#if defined(WEAVE_POSTGRES_TEST_LIVE)
static weave::Task<void> live_session(pg::Options options)
{
  co_await session(options, true);
  co_await terminal(options, "cancel_before", true);
  co_await terminal(options, "cancel_after", true);
}

int main()
{
  using namespace std::chrono_literals;
  static fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::array<std::string, 5> input;
  for (auto &line : input)
    std::getline(std::cin, line);
  std::printf("Owned certificate fixture: %s\n", certificates.directory.string().c_str());
  auto port = weave::parse_port(input[0]);
  auto address = weave::IpAddress::parse(input[3]);
  check(port && address);
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  check(credentials);
  pg::Options plain{.host = "localhost", .port = *port, .user = "weave", .database = "postgres", .plaintext = true};
  plain.password = input[1];
  plain.hosts = {{"localhost", *port, *address}};
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = pg::ChannelBinding::require;
  const std::array profiles{plain, secured};
  auto ctx = weave::Context::create();
  check(ctx);
  for (const auto &profile : profiles) {
    check(ctx->run(weave::timeout(15s, live_session(profile))));
    blocking(profile, true);
  }

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  constexpr unsigned workers = 4;
  constexpr unsigned roots = 16;
  constexpr unsigned scheduler_count = 2;
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = workers, .scheduler = scheduler});
    check(runtime);
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < roots; ++index) {
      auto job = runtime->spawn(weave::timeout(15s, live_session(profiles[index % profiles.size()])));
      check(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      check(std::move(job).get());
  }
#else
  constexpr unsigned workers = 0;
  constexpr unsigned roots = 0;
  constexpr unsigned scheduler_count = 0;
#endif
  std::printf(
    "Queueable Flush live controls passed: %u checks; workers=%u roots=%u schedulers=%u profiles=plain,mtls\n",
    checks.load(),
    workers,
    roots,
    scheduler_count);
}
#else
int main(int argc, char **argv)
{
  check(argc == 3);
  auto port = weave::parse_port(argv[1]);
  check(port);
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  std::string mode = argv[2];
  auto delimiter = mode.find(':');
  std::string scenario = delimiter == std::string::npos ? "normal" : mode.substr(delimiter + 1);
  mode.resize(delimiter == std::string::npos ? mode.size() : delimiter);
  if (mode == "blocking") {
    if (scenario == "normal")
      blocking(options);
    else
      blocking_terminal(options, scenario);
  } else if (mode == "context") {
    auto ctx = weave::Context::create();
    check(ctx);
    auto task = scenario == "normal" ? session(options) : terminal(options, scenario);
    check(ctx->run(std::move(task)));
  } else {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    auto scheduler = mode.find("stealing") != std::string::npos ? weave::Scheduler::work_stealing
                                                                : weave::Scheduler::worker_affine;
    auto io = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = io});
    check(runtime);
    std::vector<weave::JoinHandle<void>> jobs;
    const unsigned roots = scenario == "normal" ? 32 : 16;
    for (unsigned index = 0; index < roots; ++index) {
      auto task = scenario == "normal" ? session(options) : terminal(options, scenario);
      auto job = runtime->spawn(std::move(task));
      check(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      check(std::move(job).get());
#else
    check(false);
#endif
  }
  std::printf("Queueable Flush controls passed: %u checks\n", checks.load());
}
#endif
