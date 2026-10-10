#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <iostream>

namespace pg = weave::pg;
using namespace std::chrono_literals;

static void check(bool value)
{
  if (!value) {
    WEAVE_LOG_ERROR("Live notification control failed");
    std::abort();
  }
}

static weave::Task<void> notify(pg::Connection &source, std::string channel)
{
  auto sql = "DO $$ BEGIN FOR n IN 0..7 LOOP PERFORM pg_notify('" + channel + "', n::text); END LOOP; END $$";
  co_await source.query(std::move(sql));
}

static weave::Task<void> consume(pg::Connection &listener, unsigned &count, unsigned target)
{
  auto query = co_await listener.query("SELECT 42");
  check(query.front().rows.front().front().bytes() == "42");
  while (count < target) {
    auto event = co_await listener.wait_notification();
    check(event.payload == std::to_string((count - 1) % 8));
  }
  check(count == target && listener.take_notifications().empty());
}

static weave::Task<void> observe(pg::Options options)
{
  unsigned count = 0;
  auto listener = co_await pg::connect(options);
  auto source = co_await pg::connect(options);
  std::string channel = "weave_notifications_" + std::to_string(listener.backend_process());
  std::vector<pg::Notification> retained;
  auto installed = listener.on_notification([&](const pg::Notification &event) noexcept {
    check(event.channel == channel && event.process == source.backend_process());
    check(event.payload == std::to_string(count % 8));
    ++count;
    retained.push_back(event);
  });
  check(installed && !*installed);
  co_await listener.query("LISTEN " + channel);
  co_await weave::when_all(consume(listener, count, 8), notify(source, channel));
  co_await listener.reset(options);
  channel = "weave_notifications_" + std::to_string(listener.backend_process());
  co_await listener.query("LISTEN " + channel);
  co_await weave::when_all(consume(listener, count, 16), notify(source, channel));
  check(retained.size() == 16 && retained.front().payload == "0" && retained.back().payload == "7");
  auto previous = listener.on_notification({});
  check(previous && *previous);
  co_await listener.finish();
  co_await source.finish();
}

static void blocking(pg::Options options)
{
  unsigned count = 0;
  auto listener = pg::BlockingConnection::connect(options);
  check(bool(listener));
  const std::string channel = "weave_blocking_" + std::to_string(listener->backend_process());
  check(bool(listener->on_notification([&](const pg::Notification &event) noexcept {
    check(event.channel == channel && event.payload == "blocking" && event.process == listener->backend_process());
    ++count;
  })));
  check(bool(listener->query("LISTEN " + channel)));
  check(bool(listener->query("SELECT pg_notify('" + channel + "', 'blocking')")));
  if (count == 0) {
    auto event = listener->wait_notification();
    check(event && event->payload == "blocking");
  }
  check(count == 1 && listener->take_notifications().empty());
  check(bool(listener->finish()));
}

int main()
{
  fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::array<std::string, 5> input;
  for (auto &line : input)
    std::getline(std::cin, line);
  auto port = weave::parse_port(input[0]);
  auto address = weave::IpAddress::parse(input[3]);
  check(bool(port) && bool(address));
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  check(bool(credentials));
  pg::Options plain{.host = "localhost", .port = *port, .user = "weave", .database = "postgres", .plaintext = true};
  plain.password = input[1];
  plain.hosts = {{"localhost", *port, *address}};
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = pg::ChannelBinding::require;
  const std::array profiles{plain, secured};
  auto ctx = weave::Context::create();
  check(bool(ctx));
  for (const auto &profile : profiles) {
    auto result = ctx->run(weave::timeout(15s, observe(profile)));
    if (!result)
      return weave::report_error(result.error());
    blocking(profile);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(bool(runtime));
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn(weave::timeout(15s, observe(profiles[index % profiles.size()])));
      check(bool(job));
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  std::puts("Live notifications: LISTEN/NOTIFY, callbacks, reset, blocking and four-worker delivery passed");
}
