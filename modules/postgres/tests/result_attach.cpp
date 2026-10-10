#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/log.hpp>
#if defined(WEAVE_POSTGRES_TEST_LIVE)
#include <weave/timer.hpp>
#include "tls_certificates.hpp"
#include <iostream>
#endif
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <source_location>
#include <thread>

namespace pg = weave::pg;
static std::atomic<unsigned> checks = 0;
static_assert(std::same_as<decltype(std::declval<pg::Event>().result), const pg::ResultSet *>);

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Attachment check failed: %u\n", location.line());
#if defined(WEAVE_POSTGRES_TEST_LIVE)
    std::exit(1);
#else
    std::abort();
#endif
  }
}

struct State {
  std::array<pg::EventId, 4> ids;
  std::array<std::atomic<unsigned>, 4> created{};
  std::array<std::atomic<unsigned>, 4> copied{};
  std::array<std::atomic<unsigned>, 4> destroyed{};
  std::array<std::atomic<unsigned>, 4> closed{};
  std::array<std::atomic<unsigned>, 4> active{};
  std::array<bool, 4> reject{};
  bool zero_error = false;
  std::atomic<unsigned> owners{0};
  std::atomic<unsigned> data{0};
  pg::ResultSet *target = nullptr;
  pg::Connection *other = nullptr;
};

struct Owner {
  std::shared_ptr<State> state;

  explicit Owner(std::shared_ptr<State> value) : state(std::move(value))
  {
    ++state->owners;
  }

  ~Owner()
  {
    --state->owners;
  }
};

struct Data {
  std::shared_ptr<State> state;
  unsigned value;

  Data(std::shared_ptr<State> owner, unsigned number) : state(std::move(owner)), value(number)
  {
    ++state->data;
  }

  ~Data()
  {
    --state->data;
  }
};

static pg::ResultSet make_result()
{
  pg::ResultSet result;
  result.kind = pg::ResultKind::tuples;
  result.columns = {{"binary", 42, -2, 17, -1, 7, pg::Format::binary}, {"empty"}, {"null"}};
  result.rows = {{{std::string{"a\0b", 3}, pg::Format::binary}, {std::string{}}, {std::nullopt}}};
  result.command = "manual";
  result.parameter_types = {17, 25};
  result.suspended = true;
  return result;
}

static void shape(const pg::ResultSet &result)
{
  check(result.columns.size() == 3 && result.rows.size() == 1);
  check(result.columns.front().name == "binary" && result.columns.front().table == 42);
  check(result.rows.front().front().bytes() == std::string_view{"a\0b", 3});
  check(!result.rows.front()[1].is_null() && result.rows.front()[1].bytes().empty());
  check(result.rows.front()[2].is_null());
  check(result.command == "manual" && result.parameter_types == std::vector<weave::u32>{17, 25} && result.suspended);
}

static pg::EventHandler handler(std::shared_ptr<State> state, unsigned index)
{
  return [owner = std::make_unique<Owner>(state), state, index](pg::Event &event) noexcept -> weave::Result<void> {
    check(state->active[index].fetch_add(1) == 0);
    struct Active {
      std::atomic<unsigned> &value;
      ~Active()
      {
        --value;
      }
    } active{state->active[index]};
    if (event.kind == pg::EventKind::registered) {
      event.data = std::make_shared<Data>(state, 100 + index);
    } else if (event.kind == pg::EventKind::result_create) {
      ++state->created[index];
      check(event.connection && event.result && !event.source && event.source_data && *event.source_data);
      check(event.result == state->target && !event.data);
      shape(*event.result);
      auto nested = event.connection->attach_events(*state->target);
      check(!nested && nested.error() == pg::Error::busy);
      if (state->other && state->other != event.connection) {
        auto unobserved = make_result();
        auto blocked = state->other->attach_events(unobserved);
        check(!blocked && blocked.error() == pg::Error::busy);
        check(!unobserved.event_data(state->ids[0]));
      }
      event.data = std::make_shared<Data>(state, std::static_pointer_cast<Data>(*event.source_data)->value);
      if (state->reject[index]) {
        if (index == 0 && state->zero_error)
          return std::unexpected(std::error_code{});
        auto error = index == 0 ? std::errc::permission_denied : std::errc::io_error;
        return std::unexpected(std::make_error_code(error));
      }
    } else if (event.kind == pg::EventKind::result_copy) {
      ++state->copied[index];
      check(!event.connection && event.source && event.result && !event.data);
      event.data = std::make_shared<Data>(state, std::static_pointer_cast<Data>(*event.source_data)->value);
    } else if (event.kind == pg::EventKind::result_destroy) {
      ++state->destroyed[index];
      check(event.result && !event.connection && event.data);
    } else if (event.kind == pg::EventKind::connection_reset) {
      ++std::static_pointer_cast<Data>(event.data)->value;
    } else if (event.kind == pg::EventKind::connection_destroy) {
      ++state->closed[index];
    }
    return {};
  };
}

static weave::Task<pg::ResultSet> attach(pg::Options options, std::shared_ptr<State> state)
{
  auto connection = co_await pg::connect(options);
  auto result = make_result();
  state->target = &result;
  check(bool(connection.attach_events(result)) && state->owners == 0 && state->data == 0);
  for (unsigned index = 0; index < 3; ++index) {
    auto id = connection.on_event("owner-" + std::to_string(index), handler(state, index));
    check(bool(id));
    state->ids[index] = *id;
  }
  state->reject[0] = true;
  state->reject[1] = true;
  state->zero_error = true;
  {
    auto deferred = connection.query("HOLD");
    auto attached = connection.attach_events(result);
    check(!attached && attached.error() == pg::Error::busy && state->created[0] == 0);
  }
  {
    auto pipeline = connection.pipeline();
    check(bool(pipeline));
    auto attached = connection.attach_events(result);
    check(!attached && attached.error() == pg::Error::busy);
  }
  auto moved = std::move(connection);
  auto attached = moved.attach_events(result);
  check(!attached && attached.error() == std::error_code{});
  state->zero_error = false;
  auto saved = result.event_data(state->ids[2]);
  check(bool(saved));
  check(bool(result.set_event_data(state->ids[2], {})));
  check(state->created[0] == 1 && state->created[1] == 1 && state->created[2] == 1 && state->data == 4);
  check(!result.event_data(state->ids[0]) && !result.event_data(state->ids[1]));
  check(bool(result.event_data(state->ids[2])));
  auto repeated = moved.attach_events(result);
  check(!repeated && repeated.error() == std::errc::permission_denied);
  auto empty = result.event_data(state->ids[2]);
  check(empty && !*empty);
  check(bool(result.set_event_data(state->ids[2], std::move(*saved))));
  check(state->created[0] == 2 && state->created[1] == 2 && state->created[2] == 1 && state->data == 4);
  state->reject[0] = false;
  auto partial = moved.attach_events(result);
  check(!partial && partial.error() == std::errc::io_error && state->data == 5);
  state->reject[1] = false;
  check(bool(moved.attach_events(result)) && state->data == 6);
  auto counts = std::array{state->created[0].load(), state->created[1].load(), state->created[2].load()};
  check(bool(moved.attach_events(result)));
  for (unsigned index = 0; index < 3; ++index)
    check(state->created[index] == counts[index]);

  co_await moved.reset(options);
  check(std::static_pointer_cast<Data>(*result.event_data(state->ids[0]))->value == 100);
  const std::array kinds{
    pg::ResultKind::uninitialized,
    pg::ResultKind::empty_query,
    pg::ResultKind::command,
    pg::ResultKind::tuples,
    pg::ResultKind::description,
    pg::ResultKind::row_chunk,
    pg::ResultKind::acknowledgment};
  for (auto kind : kinds) {
    auto next = result.copy({.observers = false});
    next.kind = kind;
    state->target = &next;
    check(bool(moved.attach_events(next)) && next.kind == kind);
    check(std::static_pointer_cast<Data>(*next.event_data(state->ids[0]))->value == 101);
    auto copy = next;
    check(copy.kind == kind && copy.event_data(state->ids[0]));
  }
  state->target = &result;
  auto other = co_await pg::connect(options);
  state->other = &moved;
  auto foreign = other.on_event("foreign", handler(state, 3));
  check(bool(foreign));
  state->ids[3] = *foreign;
  check(bool(other.attach_events(result)) && result.event_data(*foreign));
  check(bool(moved.attach_events(result)) && bool(other.attach_events(result)));
  check(state->created[3] == 1);
  state->other = nullptr;
  check(bool(moved.close()));
  {
    auto closed_result = make_result();
    state->target = &closed_result;
    check(bool(moved.attach_events(closed_result)) && !moved.open());
    shape(closed_result);
  }
  state->target = nullptr;
  co_return result;
}

static void remaining(pg::ResultSet result, const std::shared_ptr<State> &state)
{
  check(state->owners == 4 && state->data == 4);
  for (unsigned index = 0; index < 4; ++index)
    check(state->closed[index] == 1 && result.event_data(state->ids[index]));
  std::vector<std::thread> workers;
  for (unsigned index = 0; index < 8; ++index) {
    workers.emplace_back([&result, &state] {
      for (unsigned iteration = 0; iteration < 32; ++iteration) {
        auto copy = result;
        shape(copy);
        for (auto id : state->ids)
          check(bool(copy.event_data(id)));
        auto bare = copy.copy({.observers = false});
        check(!bare.event_data(state->ids[0]));
      }
    });
  }
  for (auto &worker : workers)
    worker.join();
  check(state->data == 4 && state->owners == 4);
}

static void blocking(pg::Options options)
{
  auto state = std::make_shared<State>();
  {
    auto connection = pg::BlockingConnection::connect(options);
    check(bool(connection));
    auto result = make_result();
    state->target = &result;
    auto key = connection->on_event("blocking", handler(state, 0));
    check(bool(key));
    state->reject[0] = true;
    state->zero_error = true;
    auto rejected = connection->attach_events(result);
    check(!rejected && rejected.error() == std::error_code{} && state->data == 1);
    state->reject[0] = false;
    state->zero_error = false;
    check(bool(connection->attach_events(result)) && result.event_data(*key));
    check(bool(connection->attach_events(result)) && state->created[0] == 2);
    check(bool(connection->reset(options)));
    check(bool(connection->close()));
    auto next = make_result();
    state->target = &next;
    check(bool(connection->attach_events(next)) && next.event_data(*key));
    check(std::static_pointer_cast<Data>(*next.event_data(*key))->value == 101);
    state->target = nullptr;
  }
  check(state->owners == 0 && state->data == 0 && state->closed[0] == 1);
}

#if defined(WEAVE_POSTGRES_TEST_LIVE)
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
    auto state = std::make_shared<State>();
    auto result = ctx->run(weave::timeout(15s, attach(profile, state)));
    check(bool(result));
    remaining(std::move(*result), state);
    check(state->owners == 0 && state->data == 0);
    blocking(profile);
  }

#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  constexpr unsigned workers = 4;
  constexpr unsigned roots = 32;
  constexpr unsigned scheduler_count = 2;
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = workers, .scheduler = scheduler});
    check(bool(runtime));
    std::vector<std::shared_ptr<State>> states;
    std::vector<weave::JoinHandle<pg::ResultSet>> jobs;
    for (unsigned index = 0; index < roots; ++index) {
      auto state = std::make_shared<State>();
      auto job = runtime->spawn(weave::timeout(15s, attach(profiles[index % profiles.size()], state)));
      check(bool(job));
      states.push_back(std::move(state));
      jobs.push_back(std::move(*job));
    }
    for (std::size_t index = 0; index < jobs.size(); ++index) {
      auto result = std::move(jobs[index]).get();
      check(bool(result));
      remaining(std::move(*result), states[index]);
      check(states[index]->owners == 0 && states[index]->data == 0);
    }
  }
#else
  constexpr unsigned workers = 0;
  constexpr unsigned roots = 0;
  constexpr unsigned scheduler_count = 0;
#endif
  std::printf(
    "Application-result live controls passed: %u checks; workers=%u roots=%u schedulers=%u profiles=plain,mtls\n",
    checks.load(),
    workers,
    roots,
    scheduler_count);
}
#else
int main(int argc, char **argv)
{
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  check(argc == 3);
  auto port = weave::parse_port(argv[1]);
  check(bool(port));
  pg::Options options{.host = "127.0.0.1", .port = *port, .user = "test", .plaintext = true};
  std::string mode = argv[2];
  if (mode == "blocking") {
    blocking(options);
  } else if (mode == "context") {
    auto state = std::make_shared<State>();
    auto ctx = weave::Context::create();
    check(bool(ctx));
    auto result = ctx->run(attach(options, state));
    check(bool(result));
    remaining(std::move(*result), state);
    check(state->owners == 0 && state->data == 0);
  } else {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    auto scheduler = mode.find("stealing") != std::string::npos ? weave::Scheduler::work_stealing
                                                                : weave::Scheduler::worker_affine;
    auto io = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = io});
    check(bool(runtime));
    std::vector<std::shared_ptr<State>> states;
    std::vector<weave::JoinHandle<pg::ResultSet>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto state = std::make_shared<State>();
      auto job = runtime->spawn(attach(options, state));
      check(bool(job));
      states.push_back(std::move(state));
      jobs.push_back(std::move(*job));
    }
    for (std::size_t index = 0; index < jobs.size(); ++index) {
      auto result = std::move(jobs[index]).get();
      check(bool(result));
      remaining(std::move(*result), states[index]);
      check(states[index]->owners == 0 && states[index]->data == 0);
    }
#else
    check(false);
#endif
  }
  std::printf("Application-result attachment controls passed: %u checks\n", checks.load());
}
#endif
