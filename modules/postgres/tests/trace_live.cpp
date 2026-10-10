#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <iostream>

namespace pg = weave::pg;

static void check(bool value, const char *message)
{
  if (!value) {
    WEAVE_LOG_ERROR("Live trace: %s", message);
    std::abort();
  }
}

struct State {
  unsigned frontend_password = 0;
  unsigned backend_auth = 0;
  unsigned keys = 0;
  unsigned binds = 0;
  unsigned rows = 0;
  bool ready = false;

  void inspect(const pg::TraceMessage &message) noexcept
  {
    const bool authentication = !ready || message.kind == 0 || message.kind == 'p' || message.kind == 'R' ||
      message.kind == 'K';
    if (authentication)
      check(message.redacted && message.payload.data() == nullptr && !message.truncated, "Credentials withheld");
    else
      check(
        !message.redacted && !message.truncated && message.payload.size() + 4 == message.length,
        "Opt-in logical protocol body preserved through TLS");
    if (message.kind == 0)
      ready = false;
    frontend_password += message.direction == pg::TraceDirection::frontend && message.kind == 'p';
    backend_auth += message.direction == pg::TraceDirection::backend && message.kind == 'R';
    keys += message.kind == 'K';
    binds += message.direction == pg::TraceDirection::frontend && message.kind == 'B';
    rows += message.direction == pg::TraceDirection::backend && message.kind == 'D';
    if (message.direction == pg::TraceDirection::backend && message.kind == 'Z')
      ready = true;
  }
};

static pg::Trace observer(State &state)
{
  return {
    .handler =
      [&state](const pg::TraceMessage &message) noexcept {
        state.inspect(message);
      },
    .content = pg::TraceContent::application};
}

static weave::Task<void> asynchronous(pg::Options options)
{
  State state;
  auto connection = co_await pg::connect(options, {}, observer(state));
  check(state.frontend_password == 2 && state.backend_auth == 4 && state.keys == 1, "Actual SCRAM exchange traced");
  std::vector<pg::Parameter> parameters{{"trace-application-value"}};
  auto result = co_await connection.execute("SELECT $1::text", std::move(parameters));
  check(result.rows.front().front().bytes() == "trace-application-value", "Query unaffected");
  auto pipeline = connection.pipeline();
  check(bool(pipeline), "Pipeline");
  for (unsigned index = 0; index < 8; ++index)
    check(bool(pipeline->execute({"SELECT 42"})), "Enqueue");
  check(bool(pipeline->sync()), "Sync");
  co_await weave::when_all(pipeline->send(), pipeline->receive());
  unsigned count = 0;
  while (auto event = co_await pipeline->next()) {
    check(event->kind == pg::PipelineKind::sync || event->outcome.result.has_value(), "Actual result");
    ++count;
  }
  check(count == 9 && bool(pipeline->finish()), "Drain");
  co_await connection.reset(options);
  check(state.frontend_password == 4 && state.backend_auth == 8 && state.keys == 2, "SCRAM reset redacts anew");
  co_await connection.finish();
  check(state.binds == 9 && state.rows == 9, "Application queries traced");
}

static void blocking(pg::Options options)
{
  State state;
  auto connection = pg::BlockingConnection::connect(options, {}, observer(state));
  check(bool(connection), "Blocking connect");
  check(bool(connection->execute("SELECT $1::text", {{"trace-application-value"}})), "Blocking query");
  check(bool(connection->reset(options)) && bool(connection->finish()), "Blocking reset and finish");
  check(
    state.frontend_password == 4 && state.backend_auth == 8 && state.keys == 2 && state.rows == 1,
    "Blocking actual SCRAM controls");
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
  check(bool(port) && bool(address), "Owned deployment input");
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  check(bool(credentials), "Verified mTLS credentials");
  pg::Options plain{.host = "localhost", .port = *port, .user = "weave", .database = "postgres", .plaintext = true};
  plain.hosts = {{"localhost", *port, *address}};
  plain.password = input[1];
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = pg::ChannelBinding::require;
  auto ctx = weave::Context::create();
  check(bool(ctx), "Context");
  const std::array profiles{plain, secured};
  for (const auto &profile : profiles) {
    auto result = ctx->run(asynchronous(profile));
    if (!result)
      return weave::report_error(result.error());
    blocking(profile);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(bool(runtime), "Runtime");
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn(asynchronous(profiles[index % profiles.size()]));
      check(bool(job), "Submission");
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  std::puts("Live trace: plaintext SCRAM and verified mTLS SCRAM-PLUS passed");
}
