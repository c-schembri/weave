#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <atomic>
#include <thread>
#include <source_location>
#include <algorithm>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks = 0;

struct ThrowingHandler {
  weave::Result<void> operator()(pg::Event &);
};

static_assert(!std::constructible_from<pg::EventHandler, ThrowingHandler>);
static_assert(!std::copy_constructible<pg::EventHandler>);
static_assert(std::is_nothrow_move_constructible_v<pg::ResultSet>);
static_assert(std::is_nothrow_move_assignable_v<pg::ResultSet>);

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    WEAVE_LOG_ERROR("Lifecycle control failed at line %u", location.line());
    std::abort();
  }
}

struct Stats {
  pg::EventId key;
  std::atomic<unsigned> registered{0};
  std::atomic<unsigned> reset{0};
  std::atomic<unsigned> closed{0};
  std::atomic<unsigned> created{0};
  std::atomic<unsigned> copied{0};
  std::atomic<unsigned> destroyed{0};
  std::atomic<unsigned> lifetime{0};
  std::atomic<unsigned> active{0};
  std::atomic<unsigned> payloads{0};
};

struct Lifetime {
  std::shared_ptr<Stats> stats;

  explicit Lifetime(std::shared_ptr<Stats> state) : stats(std::move(state))
  {
  }

  ~Lifetime()
  {
    ++stats->lifetime;
  }
};

struct Payload {
  std::shared_ptr<Stats> stats;
  unsigned value;

  Payload(std::shared_ptr<Stats> state, unsigned number) : stats(std::move(state)), value(number)
  {
    ++stats->payloads;
  }

  ~Payload()
  {
    --stats->payloads;
  }
};

static pg::EventHandler handler(std::shared_ptr<Stats> stats)
{
  return [owner = std::make_unique<Lifetime>(stats), stats](pg::Event &event) noexcept -> weave::Result<void> {
    check(stats->active.fetch_add(1) == 0);
    struct Active {
      Stats &stats;
      ~Active()
      {
        --stats.active;
      }
    } active{*stats};
    check(event.name == "audit" && event.id.value != 0);
    if (event.connection) {
      auto &connection = *event.connection;
      auto reject = [](pg::Event &) noexcept -> weave::Result<void> {
        return {};
      };
      auto registered = connection.on_event("nested", reject);
      auto closed = connection.close();
      auto cancelled = connection.cancel();
      auto changed = connection.set_event_data(event.id, {});
      auto notices = connection.on_notice({});
      auto notifications = connection.on_notification({});
      auto trace = connection.on_trace({});
      auto pipeline = connection.pipeline();
      check(!registered && registered.error() == pg::Error::busy);
      check(!closed && closed.error() == pg::Error::busy);
      check(!cancelled && cancelled.error() == pg::Error::busy);
      check(!changed && changed.error() == pg::Error::busy);
      check(!notices && !notifications && !trace && !pipeline);
    }
    switch (event.kind) {
    case pg::EventKind::registered:
      ++stats->registered;
      check(event.connection && !event.result && !event.data);
      event.data = std::make_shared<Payload>(stats, 42);
      break;
    case pg::EventKind::connection_reset:
      ++stats->reset;
      check(event.connection && event.connection->open() && event.data);
      ++std::static_pointer_cast<Payload>(event.data)->value;
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
    case pg::EventKind::connection_destroy:
      ++stats->closed;
      check(event.connection && !event.result && event.data);
      break;
    case pg::EventKind::result_create:
      ++stats->created;
      check(event.connection && event.result && !event.source && event.source_data && *event.source_data);
      check(!event.data);
      event.data = std::make_shared<Payload>(stats, std::static_pointer_cast<Payload>(*event.source_data)->value);
      break;
    case pg::EventKind::result_copy:
      ++stats->copied;
      check(!event.connection && event.result && event.source && event.source_data && *event.source_data);
      check(event.result->command == event.source->command && !event.data);
      check(event.result->kind == event.source->kind && event.result->suspended == event.source->suspended);
      check(event.result->parameter_types == event.source->parameter_types);
      event.data = std::make_shared<Payload>(stats, std::static_pointer_cast<Payload>(*event.source_data)->value);
      break;
    case pg::EventKind::result_destroy:
      ++stats->destroyed;
      check(!event.connection && event.result && !event.source && event.data);
      break;
    }
    return {};
  };
}

static weave::Task<void> copy_send(pg::Connection &connection)
{
  std::array<std::byte, 512> data;
  data.fill(std::byte{'y'});
  co_await connection.write_copy(data);
  co_await connection.write_copy(data);
  co_await connection.finish_copy_send();
}

static weave::Task<void> copy_read(pg::Connection &connection)
{
  unsigned count = 0;
  while (auto data = co_await connection.read_copy()) {
    check(data->size() == 512 && data->front() == std::byte{'x'});
    ++count;
  }
  check(count == 2);
}

static weave::Task<pg::ResultSet> observe(pg::Options options, std::shared_ptr<Stats> stats)
{
  auto connection = co_await pg::connect(options);
  auto key = connection.on_event("audit", handler(stats));
  if (key)
    stats->key = *key;
  check(bool(key) && stats->registered == 1 && stats->lifetime == 0);
  auto duplicate = connection.on_event("audit", handler(stats));
  check(!duplicate && duplicate.error() == std::errc::file_exists && stats->lifetime == 1);
  check(!connection.on_event("", {}) && !connection.event_data({}));
  {
    auto unstarted = connection.query("HOLD");
    check(!connection.on_event("deferred", {}) && !connection.set_event_data(*key, {}));
  }
  auto state = connection.event_data(*key);
  check(state && std::static_pointer_cast<Payload>(*state)->value == 42);
  state = pg::EventData{};
  auto result = co_await connection.execute("SELECT 1");
  check(stats->created == 1 && result.command == "SELECT 1");
  check(bool(result.event_data(*key)) && !result.event_data({}));
  {
    auto copy = result;
    check(
      stats->copied == 1 && copy.event_data(*key) && copy.event_data(*key)->get() != result.event_data(*key)->get());
    auto moved = std::move(copy);
    check(stats->copied == 1 && !copy.event_data(*key) && moved.event_data(*key));
    moved = moved;
    check(stats->copied == 1 && stats->destroyed == 0);
    moved = result;
    check(stats->copied == 2 && stats->destroyed == 1);
    auto transfer = result;
    moved = std::move(transfer);
    check(stats->copied == 3 && stats->destroyed == 2 && !transfer.event_data(*key));
    auto *self = &moved;
    moved = std::move(*self);
    check(stats->copied == 3 && stats->destroyed == 2);
    check(bool(moved.set_event_data(*key, std::make_shared<Payload>(stats, 99))));
    check(std::static_pointer_cast<Payload>(*moved.event_data(*key))->value == 99);
    check(std::static_pointer_cast<Payload>(*result.event_data(*key))->value == 42);
  }
  check(stats->destroyed == 3);
  std::vector<pg::Command> commands{{"SELECT 1"}, {"SELECT 1"}};
  auto batch = co_await connection.batch(std::move(commands));
  check(batch.size() == 2 && batch[0].result && batch[1].result);
  check(batch[0].result->event_data(*key) && batch[1].result->event_data(*key));
  auto pipeline = connection.pipeline();
  check(bool(pipeline) && !connection.set_event_data(*key, {}));
  for (unsigned index = 0; index < 8; ++index)
    check(bool(pipeline->execute({"SELECT 1"})));
  check(bool(pipeline->sync()));
  co_await weave::when_all(pipeline->send(), pipeline->receive());
  unsigned delivered = 0;
  while (auto event = co_await pipeline->next()) {
    if (event->outcome.result) {
      check(bool(event->outcome.result->event_data(*key)));
      ++delivered;
    }
  }
  check(delivered == 8 && pipeline->finish());
  auto format = co_await connection.start_copy("COPY BOTH");
  check(format.direction == pg::CopyDirection::both);
  co_await weave::when_all(copy_send(connection), copy_read(connection));
  auto copied = co_await connection.end_copy();
  check(copied.command == "COPY 2" && copied.event_data(*key));
  check(connection.copy_result()->event_data(*key) && connection.copy_results()->front().event_data(*key));
  auto exchange = co_await connection.exchange("HOLD");
  auto item = co_await exchange.next();
  check(item && std::get<pg::ResultSet>(*item).event_data(*key));
  check(!(co_await exchange.next()) && exchange.finish());
  auto created = stats->created.load();
  auto failed = co_await weave::as_result(connection.query("ERROR"));
  check(!failed && stats->created == created && connection.open());
  auto moved = std::move(connection);
  co_await moved.reset(options);
  check(stats->reset == 1 && stats->closed == 0);
  auto invalid = options;
  invalid.user = "bad";
  check(!(co_await weave::as_result(moved.reset(invalid))));
  check(stats->reset == 1 && stats->closed == 0);
  co_await moved.reset(options);
  check(stats->reset == 2);
  auto after = co_await moved.query("HOLD");
  check(std::static_pointer_cast<Payload>(*after.front().event_data(*key))->value == 44);
  check(std::static_pointer_cast<Payload>(*result.event_data(*key))->value == 42);
  check(bool(moved.close()) && stats->closed == 0);
  co_return result;
}

static weave::Task<void> reject(pg::Options options)
{
  unsigned registration_destroyed = 0;
  unsigned creates = 0;
  unsigned copies = 0;
  unsigned destroyed = 0;
  auto connection = co_await pg::connect(options);
  auto rejected = connection.on_event(
    "rejected",
    [state = std::make_unique<unsigned>(7), &registration_destroyed](pg::Event &event) noexcept -> weave::Result<void> {
      if (event.kind == pg::EventKind::connection_destroy)
        ++registration_destroyed;
      event.data = std::make_shared<unsigned>(3);
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
    });
  check(!rejected && rejected.error() == std::errc::permission_denied && registration_destroyed == 0);
  auto first = connection.on_event("create-reject", [&](pg::Event &event) noexcept -> weave::Result<void> {
    if (event.kind == pg::EventKind::result_create) {
      ++creates;
      event.data = std::make_shared<unsigned>(1);
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
    }
    if (event.kind == pg::EventKind::result_copy)
      ++copies;
    if (event.kind == pg::EventKind::result_destroy)
      ++destroyed;
    return {};
  });
  auto second = connection.on_event("copy-reject", [&](pg::Event &event) noexcept -> weave::Result<void> {
    if (event.kind == pg::EventKind::result_create)
      event.data = std::make_shared<unsigned>(2);
    if (event.kind == pg::EventKind::result_copy) {
      ++copies;
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
    }
    if (event.kind == pg::EventKind::result_destroy)
      ++destroyed;
    return {};
  });
  auto shared = connection.on_event("shared", [](pg::Event &event) noexcept -> weave::Result<void> {
    if (event.kind == pg::EventKind::result_create)
      event.data = std::make_shared<unsigned>(7);
    if (event.kind == pg::EventKind::result_copy)
      event.data = *event.source_data;
    return {};
  });
  check(first && second && shared && *first != *second);
  {
    auto results = co_await connection.query("HOLD");
    check(creates == 1 && !results.front().event_data(*first) && results.front().event_data(*second));
    auto copy = results.front().copy({.rows = false});
    auto next = copy.copy();
    check(copies == 1 && !copy.event_data(*second) && !next.event_data(*second));
    check(copy.event_data(*shared)->get() == results.front().event_data(*shared)->get());
    check(next.event_data(*shared)->get() == copy.event_data(*shared)->get());
  }
  check(destroyed == 1 && registration_destroyed == 0);
}

static void selective(const pg::ResultSet &source, const std::shared_ptr<Stats> &stats)
{
  const std::array selections{false, true};
  for (bool columns : selections) {
    for (bool rows : selections) {
      for (bool observers : selections) {
        auto copied = stats ? stats->copied.load() : 0;
        auto result = source.copy({.columns = columns, .rows = rows, .observers = observers});
        check(result.kind == source.kind && result.command == source.command);
        check(result.parameter_types == source.parameter_types && result.suspended == source.suspended);
        check(result.columns.size() == ((columns || rows) ? source.columns.size() : 0));
        check(result.rows.size() == (rows ? source.rows.size() : 0));
        if (columns || rows) {
          const auto &actual = result.columns.front();
          const auto &expected = source.columns.front();
          check(
            actual.name == expected.name && actual.table == expected.table && actual.attribute == expected.attribute);
          check(actual.type == expected.type && actual.type_size == expected.type_size);
          check(actual.modifier == expected.modifier && actual.format == expected.format);
          result.columns.front().name = "mutated";
          check(source.columns.front().name != "mutated");
        }
        if (rows) {
          for (std::size_t row = 0; row < source.rows.size(); ++row) {
            check(result.rows[row].size() == source.rows[row].size());
            for (std::size_t index = 0; index < source.rows[row].size(); ++index) {
              const auto &actual = result.rows[row][index];
              const auto &expected = source.rows[row][index];
              check(actual.data == expected.data && actual.format == expected.format);
            }
          }
          result.rows.front().front().data = "mutated";
          check(source.rows.front().front().data != "mutated");
        }
        if (stats) {
          // The observer counters are checked only on the serial control path.
          check(stats->copied == copied + static_cast<unsigned>(observers));
          auto state = result.event_data(stats->key);
          check(state.has_value() == observers);
          if (observers)
            check(state->get() != source.event_data(stats->key)->get());
          else
            check(state.error() == std::errc::invalid_argument);
        }
      }
    }
  }
}

static void fill(pg::ResultSet &result)
{
  result.columns = {
    {"binary", 42, -2, 17, -1, 7, pg::Format::binary},
    {"empty", 43, 2, 25, -1, -1, pg::Format::text},
    {"null", 0, 0, 25, -1, -1, pg::Format::text}};
  result.rows = {
    {{std::string{"a\0b", 3}, pg::Format::binary},
      {std::string{}, pg::Format::text},
      {std::nullopt, pg::Format::text}}};
  result.parameter_types = {17, 25};
  result.suspended = true;
}

static pg::ResultSet remaining(pg::ResultSet result, const std::shared_ptr<Stats> &stats)
{
  check(stats->closed == 1 && stats->lifetime == 1);
  fill(result);
  selective(result, stats);
  std::vector<std::thread> workers;
  for (unsigned index = 0; index < 8; ++index) {
    workers.emplace_back([&result] {
      for (unsigned repeat = 0; repeat < 32; ++repeat) {
        auto complete = result;
        auto transferred = std::move(complete);
        check(transferred.command == result.command && transferred.rows.size() == result.rows.size());

        auto copy = result.copy(
          {.columns = (repeat & 1) != 0, .rows = (repeat & 2) != 0, .observers = (repeat & 4) != 0});
        auto next = std::move(copy);
        check(next.command == result.command && next.kind == result.kind);
        check(next.rows.size() == ((repeat & 2) != 0 ? 1 : 0));
      }
    });
  }
  for (auto &worker : workers)
    worker.join();
  check(stats->active == 0);
  return result.copy({.observers = false});
}

static weave::Result<void> blocking(pg::Options options)
{
  auto stats = std::make_shared<Stats>();
  auto connection = pg::BlockingConnection::connect(options);
  if (!connection)
    return std::unexpected(connection.error());
  auto key = connection->on_event("audit", handler(stats));
  if (key)
    stats->key = *key;
  check(bool(key) && connection->event_data(*key));
  auto result = connection->execute("SELECT 1");
  check(result && result->event_data(*key));
  fill(*result);
  selective(*result, stats);
  check(bool(connection->reset(options)) && stats->reset == 1);
  check(bool(connection->set_event_data(*key, std::make_shared<Payload>(stats, 70))));
  auto query = connection->query("HOLD");
  check(query && std::static_pointer_cast<Payload>(*query->front().event_data(*key))->value == 70);
  return {};
}

static weave::Task<void> reentrant(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto key = connection.on_event("reentrant", [](pg::Event &event) noexcept -> weave::Result<void> {
    if (event.kind == pg::EventKind::result_create) {
      std::puts("Observed result reentrancy reached");
      std::fflush(stdout);
      auto copy = event.result->copy({.columns = false, .rows = false, .observers = false});
    }
    return {};
  });
  check(bool(key));
  co_await connection.execute("SELECT 1");
  check(false);
}

int main(int argc, char **argv)
{
#if defined(_MSC_VER)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  pg::ResultSet unobserved;
  auto empty = unobserved.copy();
  check(empty.kind == pg::ResultKind::uninitialized && empty.columns.empty() && empty.rows.empty());
  check(empty.command.empty() && empty.parameter_types.empty() && !empty.suspended);
  fill(unobserved);
  auto large = unobserved.rows.front();
  large.front().data = std::string(65536, 'x');
  unobserved.rows.push_back(std::move(large));
  const std::array kinds{
    pg::ResultKind::uninitialized,
    pg::ResultKind::empty_query,
    pg::ResultKind::command,
    pg::ResultKind::tuples,
    pg::ResultKind::description,
    pg::ResultKind::row_chunk,
    pg::ResultKind::acknowledgment};
  for (auto kind : kinds) {
    unobserved.kind = kind;
    selective(unobserved, {});
  }
  check(argc == 3);
  auto port = weave::parse_port(argv[1]);
  check(bool(port));
  pg::Options options;
  options.host = "127.0.0.1";
  options.port = *port;
  options.user = "test";
  options.plaintext = true;
  std::string mode = argv[2];
  if (mode == "blocking") {
    auto result = blocking(options);
    if (!result)
      return weave::report_error(result.error());
    std::printf("Selective-copy controls passed: %u checks\n", checks.load());
    return 0;
  }
  if (mode == "reject" || mode == "reentrant") {
    auto ctx = weave::Context::create();
    check(bool(ctx));
    auto result = ctx->run(mode == "reject" ? reject(options) : reentrant(options));
    if (!result)
      return weave::report_error(result.error());
    std::printf("Selective-copy controls passed: %u checks\n", checks.load());
    return 0;
  }
  auto stats = std::make_shared<Stats>();
  {
    if (mode == "context") {
      auto ctx = weave::Context::create();
      check(bool(ctx));
      auto result = ctx->run(observe(options, stats));
      if (!result)
        return weave::report_error(result.error());
      auto bare = remaining(std::move(*result), stats);
      check(stats->lifetime == 2 && bare.rows.front().front().bytes() == std::string_view{"a\0b", 3});
      check(!bare.event_data(stats->key));
    } else {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
      const std::array runtime_modes{"affine", "stealing", "shared_affine", "shared_stealing"};
      check(std::ranges::find(runtime_modes, mode) != runtime_modes.end());
      auto scheduler = mode.find("stealing") != std::string::npos ? weave::Scheduler::work_stealing
                                                                  : weave::Scheduler::worker_affine;
      auto io = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = io});
      check(bool(runtime));
      std::vector<std::shared_ptr<Stats>> states;
      std::vector<weave::JoinHandle<pg::ResultSet>> jobs;
      for (unsigned root = 0; root < 32; ++root) {
        auto state = root == 0 ? stats : std::make_shared<Stats>();
        auto job = runtime->spawn(observe(options, state));
        check(bool(job));
        states.push_back(std::move(state));
        jobs.push_back(std::move(*job));
      }
      for (std::size_t root = 0; root < jobs.size(); ++root) {
        auto result = std::move(jobs[root]).get();
        if (!result)
          return weave::report_error(result.error());
        auto bare = remaining(std::move(*result), states[root]);
        check(bare.rows.front().front().bytes() == std::string_view{"a\0b", 3});
        check(!bare.event_data(states[root]->key));
        check(states[root]->lifetime == 2 && states[root]->payloads == 0);
        check(states[root]->created + states[root]->copied == states[root]->destroyed);
      }
#else
      check(false);
#endif
    }
  }
  check(stats->lifetime == 2 && stats->payloads == 0);
  check(stats->created + stats->copied == stats->destroyed);
  std::printf(
    "Selective-copy controls passed: %u checks; %u creates, %u copies\n",
    checks.load(),
    stats->created.load(),
    stats->copied.load());
}
