#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <array>
#include <atomic>
#include <string>

namespace pg = weave::pg;
using namespace std::chrono_literals;

static void check(bool value, const char *message)
{
  if (!value) {
    WEAVE_LOG_ERROR("Trace session: %s", message);
    std::abort();
  }
}

struct Observer {
  std::atomic<bool> executing{false};
  std::array<unsigned, 256> frontend{};
  std::array<unsigned, 256> backend{};
  unsigned startup = 0;
  unsigned disclosed = 0;
  unsigned *released;

  explicit Observer(unsigned &destroyed) : released(&destroyed)
  {
  }

  ~Observer()
  {
    ++*released;
  }

  void inspect(const pg::TraceMessage &message) noexcept
  {
    check(!executing.exchange(true), "Callbacks for one connection are serialized");
    check(message.length >= 4, "Framing retains wire length");
    auto &counts = message.direction == pg::TraceDirection::frontend ? frontend : backend;
    ++counts[static_cast<unsigned char>(message.kind)];
    const bool sensitive = message.kind == 0 || message.kind == 'p' || message.kind == 'R' || message.kind == 'K';
    if (sensitive) {
      check(message.redacted && message.payload.data() == nullptr && !message.truncated, "Authentication hidden");
      startup += message.kind == 0;
    } else if (!message.redacted) {
      check(message.payload.size() <= 8, "Application disclosure is bounded");
      check(message.length == message.payload.size() + 4 || message.truncated, "Truncation is explicit");
      ++disclosed;
    }
    executing.store(false);
  }
};

static pg::Trace observe(std::shared_ptr<Observer> state)
{
  return {
    .handler =
      [state = std::move(state)](const pg::TraceMessage &message) noexcept {
        state->inspect(message);
      },
    .content = pg::TraceContent::application,
    .payload_bytes = 8};
}

static weave::Task<void> send_copy(pg::Connection &connection)
{
  std::vector<std::byte> data(128 * 1024, std::byte{'y'});
  co_await weave::when_all(connection.write_copy(data), connection.write_copy(data));
  co_await connection.finish_copy_send();
}

static weave::Task<void> read_copy(pg::Connection &connection)
{
  std::size_t size = 0;
  while (auto data = co_await connection.read_copy()) {
    check(data->front() == std::byte{'x'}, "COPY data preserved");
    size += data->size();
  }
  check(size == 2 * 128 * 1024, "Duplex reader drains exactly");
}

static weave::Task<void> asynchronous(pg::Options options)
{
  unsigned released = 0;
  auto state = std::make_shared<Observer>(released);
  auto connection = co_await pg::connect(options, {}, observe(state));
  check(state->startup == 1 && state->backend['K'] == 1, "Startup traced and key withheld");

  auto failed_options = options;
  failed_options.user = "bad";
  auto failed = co_await weave::as_result(connection.reset(std::move(failed_options)));
  check(!failed && pg::sqlstate(failed.error()) == "28P01" && !connection.open(), "Failed reset is terminal");
  check(state->startup == 2 && released == 0, "Failed reset retains observer");
  co_await connection.reset(options);
  check(state->startup == 3 && state->backend['K'] == 2, "Successful reset reuses observer");

  auto pipeline = connection.pipeline();
  check(bool(pipeline), "Pipeline lease");
  auto rejected = connection.on_trace({});
  check(!rejected && rejected.error() == pg::Error::busy, "Pipeline owns registration");
  for (unsigned index = 0; index < 32; ++index)
    check(bool(pipeline->execute({"SELECT 1"})), "Queue extended command");
  check(bool(pipeline->sync()), "Queue explicit Sync");
  co_await weave::when_all(pipeline->send(), pipeline->receive());
  unsigned outcomes = 0;
  while (auto result = co_await pipeline->next()) {
    check(result->kind == pg::PipelineKind::sync || result->outcome.result.has_value(), "Pipeline outcome");
    ++outcomes;
  }
  check(outcomes == 33 && pipeline->finish().has_value(), "Pipeline drained");
  const std::array commands{'P', 'B', 'D', 'E'};
  for (char command : commands)
    check(state->frontend[command] == 32, "Coalesced request traced as individual frames");
  check(state->frontend['S'] == 1 && state->backend['C'] == 32, "Correlated protocol barriers");

  auto copy = co_await connection.start_copy("COPY BOTH");
  check(copy.direction == pg::CopyDirection::both, "Duplex COPY");
  co_await weave::when_all(send_copy(connection), read_copy(connection));
  co_await connection.end_copy();
  check(state->frontend['d'] == 2 && state->backend['d'] == 2, "Independent COPY directions traced");
  check(state->frontend['c'] == 1 && state->backend['c'] == 1, "Independent completion frames");

  auto invalid = connection.on_trace({.payload_bytes = 16 * 1024 * 1024 + 1});
  check(!invalid && invalid.error() == std::errc::invalid_argument, "Invalid payload bound leaves registration");
  const auto before = state->backend['C'];
  auto malformed = co_await weave::as_result(connection.query("MALFORMED"));
  check(!malformed && malformed.error() == pg::Error::protocol, "Malformed wire length rejected");
  check(state->backend['C'] == before, "Incomplete invalid frames never traced");
  co_await connection.reset(options);

  auto cancellation = co_await weave::as_result(weave::timeout(20ms, connection.query("CANCEL")));
  check(!cancellation && cancellation.error() == std::errc::timed_out && !connection.open(), "Cancellation drains");
  check(state->frontend['Q'] == 3, "COPY, malformed and cancelled writes submitted exactly once");
  check(state->disclosed > 150, "Application content opt-in actually delivers frames");
  auto saved = connection.on_trace({});
  check(saved && saved->handler, "Return owning callback");
  state.reset();
  check(released == 0, "Previous registration retains owner");
  saved->handler = {};
  check(released == 1, "One final owner released after cancellation drain");
}

static void blocking(pg::Options options)
{
  unsigned frames = 0;
  auto connection = pg::BlockingConnection::connect(options, {}, {.handler = [&frames](const auto &message) noexcept {
    ++frames;
    check(message.redacted && message.payload.data() == nullptr, "Blocking defaults to metadata only");
  }});
  check(bool(connection) && frames == 6, "Blocking observes full startup");
  auto pipeline = connection->pipeline();
  check(bool(pipeline), "Blocking pipeline");
  check(bool(pipeline->execute({"SELECT 1"})) && bool(pipeline->sync()), "Blocking enqueue");
  check(bool(pipeline->flush()), "Blocking duplex flush");
  unsigned count = 0;
  for (;;) {
    auto result = pipeline->next();
    check(bool(result), "Blocking next");
    if (!*result)
      break;
    ++count;
  }
  check(count == 2 && bool(pipeline->finish()), "Blocking drain");
  check(frames > 12 && bool(connection->reset(options)), "Blocking trace reset");
  unsigned zero = 0;
  auto previous = connection->on_trace(
    {.handler =
        [&zero](const auto &message) noexcept {
          ++zero;
          check(message.payload.empty() && !message.redacted, "Zero cap is explicit opt-in");
          check(message.truncated == (message.length > 4), "Zero cap reports omitted tail");
        },
      .content = pg::TraceContent::application,
      .payload_bytes = 0});
  check(previous && previous->handler, "Blocking owning save");
  check(bool(connection->query("SELECT 1")) && zero == 3, "Blocking application zero cap");
  check(bool(connection->on_trace({})) && bool(connection->finish()), "Blocking disable before terminate");
}

int main(int argc, char **argv)
{
  check(argc == 3, "port and execution mode required");
  auto port = weave::parse_port(argv[1]);
  check(bool(port), "Port");
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "weave", .plaintext = true};
  options.password = "trace-password";
  options.allow_md5_password = true;
  options.channel_binding = pg::ChannelBinding::disable;
  const std::string mode = argv[2];
  if (mode == "blocking") {
    blocking(options);
  } else if (mode == "context") {
    auto ctx = weave::Context::create();
    check(bool(ctx), "Context");
    auto result = ctx->run(weave::timeout(15s, asynchronous(options)));
    if (!result)
      return weave::report_error(result.error());
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  else if (mode == "affine" || mode == "stealing" || mode == "shared_affine" || mode == "shared_stealing") {
    auto scheduler = mode.ends_with("affine") ? weave::Scheduler::worker_affine : weave::Scheduler::work_stealing;
    auto layout = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
    check(bool(runtime), "Runtime");
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn(weave::timeout(15s, asynchronous(options)));
      check(bool(job), "Submission");
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  } else {
    check(false, "Unknown execution mode");
  }
#else
  else {
    check(false, "Runtime mode requires the Runtime module");
  }
#endif
  WEAVE_LOG_INFO("Trace mode %s passed", mode.c_str());
}
