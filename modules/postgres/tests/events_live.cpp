#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <iostream>
#include <atomic>
#include <source_location>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks = 0;

static void check(bool value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    WEAVE_LOG_ERROR("Live lifecycle control failed at line %u", location.line());
    std::exit(1);
  }
}

struct Counts {
  unsigned reset = 0;
  unsigned closed = 0;
  unsigned created = 0;
  unsigned copied = 0;
  unsigned destroyed = 0;
};

static pg::EventHandler observer(std::shared_ptr<Counts> counts)
{
  return [counts](pg::Event &event) noexcept -> weave::Result<void> {
    switch (event.kind) {
    case pg::EventKind::registered:
      event.data = std::make_shared<unsigned>(99);
      break;
    case pg::EventKind::connection_reset:
      ++counts->reset;
      check(event.connection && event.connection->open());
      check(*std::static_pointer_cast<unsigned>(event.data) == 99);
      break;
    case pg::EventKind::connection_destroy:
      ++counts->closed;
      break;
    case pg::EventKind::result_create:
      ++counts->created;
      check(event.connection && event.result && event.source_data);
      check(*std::static_pointer_cast<unsigned>(*event.source_data) == 99);
      event.data = std::make_shared<std::string>(event.result->command);
      break;
    case pg::EventKind::result_copy:
      ++counts->copied;
      check(event.source && event.result && !event.connection && event.source_data);
      event.data = std::make_shared<std::string>(*std::static_pointer_cast<std::string>(*event.source_data));
      break;
    case pg::EventKind::result_destroy:
      ++counts->destroyed;
      check(!event.connection && event.result && event.data);
      break;
    }
    return {};
  };
}

static void copies(const pg::ResultSet &source, pg::EventId key, const std::shared_ptr<Counts> &counts)
{
  const std::array selections{false, true};
  for (bool columns : selections) {
    for (bool rows : selections) {
      for (bool observers : selections) {
        auto before = counts->copied;
        auto result = source.copy({.columns = columns, .rows = rows, .observers = observers});
        check(result.kind == source.kind && result.command == source.command && result.suspended == source.suspended);
        check(result.parameter_types == source.parameter_types);
        check(result.columns.size() == ((columns || rows) ? source.columns.size() : 0));
        check(result.rows.size() == (rows ? source.rows.size() : 0));
        check(counts->copied == before + static_cast<unsigned>(observers));
        check(result.event_data(key).has_value() == observers);
        if (observers)
          check(result.event_data(key)->get() != source.event_data(key)->get());
        if (rows) {
          for (std::size_t row = 0; row < source.rows.size(); ++row) {
            check(result.rows[row].size() == source.rows[row].size());
            for (std::size_t column = 0; column < source.rows[row].size(); ++column) {
              check(result.rows[row][column].data == source.rows[row][column].data);
              check(result.rows[row][column].format == source.rows[row][column].format);
            }
          }
        }
      }
    }
  }
}

static weave::Task<pg::ResultSet> observe(pg::Options options, std::shared_ptr<Counts> counts)
{
  auto connection = co_await pg::connect(options);
  auto key = connection.on_event("audit", observer(counts));
  check(bool(key));
  auto result = co_await connection.execute("SELECT g FROM generate_series(1, 7) g");
  check(result.rows.size() == 7 && result.columns.size() == 1 && result.event_data(*key));
  copies(result, *key, counts);
  std::vector<weave::u32> types{23};
  co_await connection.prepare("statement", "SELECT $1::int", std::move(types));
  auto description = co_await connection.describe("statement");
  check(description.columns.size() == 1 && description.parameter_types == std::vector<weave::u32>{23});
  check(bool(description.event_data(*key)));
  copies(description, *key, counts);
  std::vector<pg::Parameter> parameters{{"42"}};
  auto executed = co_await connection.execute_prepared("statement", std::move(parameters));
  check(executed.rows.front().front().bytes() == "42" && executed.event_data(*key));
  copies(executed, *key, counts);
  auto multiple = co_await connection.query("SELECT 1; SELECT 2");
  check(multiple.size() == 2 && multiple[0].event_data(*key) && multiple[1].event_data(*key));
  auto empty = co_await connection.query("");
  check(empty.size() == 1 && empty.front().command.empty() && empty.front().event_data(*key));
  copies(empty.front(), *key, counts);
  std::vector<pg::Command> batch_commands{{"SELECT 1"}, {"SELECT 2"}};
  auto batch = co_await connection.batch(std::move(batch_commands));
  check(batch.size() == 2 && batch[0].result->event_data(*key) && batch[1].result->event_data(*key));

  auto pipeline = connection.pipeline();
  check(bool(pipeline));
  check(bool(pipeline->execute({"SELECT g FROM generate_series(1, 7) g"}, {.chunk_rows = 2})));
  check(bool(pipeline->sync()));
  co_await pipeline->flush();
  unsigned rows = 0;
  unsigned chunks = 0;
  while (auto event = co_await pipeline->next()) {
    if (event->outcome.result) {
      check(bool(event->outcome.result->event_data(*key)));
      copies(*event->outcome.result, *key, counts);
      rows += static_cast<unsigned>(event->outcome.result->rows.size());
      ++chunks;
    }
  }
  check(rows == 7 && chunks >= 4 && pipeline->finish());

  auto exchange = co_await connection.exchange("SELECT 1; COPY (SELECT 2) TO STDOUT; SELECT 3");
  unsigned commands = 0;
  unsigned formats = 0;
  unsigned data = 0;
  while (auto event = co_await exchange.next()) {
    if (auto *value = std::get_if<pg::ResultSet>(&*event)) {
      check(bool(value->event_data(*key)));
      ++commands;
    }
    if (std::holds_alternative<pg::CopyFormat>(*event))
      ++formats;
    if (std::holds_alternative<std::vector<std::byte>>(*event))
      ++data;
  }
  check(commands == 3 && formats == 1 && data > 0 && exchange.finish());
  auto format = co_await connection.start_copy("COPY (SELECT g FROM generate_series(1, 3) g) TO STDOUT");
  check(format.direction == pg::CopyDirection::output);
  while (auto bytes = co_await connection.read_copy())
    check(!bytes->empty());
  auto copy = connection.copy_result();
  check(copy && copy->command == "COPY 3" && copy->event_data(*key));
  copies(*copy, *key, counts);
  check(bool(connection.copy_result()->event_data(*key)));

  auto creates = counts->created;
  auto failed = co_await weave::as_result(connection.query("SELECT 1; SELECT 1/0"));
  check(!failed && pg::sqlstate(failed.error()) == "22012" && connection.open());
  check(counts->created == creates + 1);
  co_await connection.reset(options);
  check(counts->reset == 1 && counts->closed == 0);
  auto invalid = options;
  invalid.password = "not the actual password";
  check(!(co_await weave::as_result(connection.reset(invalid))));
  check(counts->reset == 1 && counts->closed == 0);
  co_await connection.reset(options);
  check(counts->reset == 2);
  auto after = co_await connection.query("SELECT 99");
  check(after.front().event_data(*key) && result.event_data(*key));
  co_return result;
}

static void verify(weave::Result<pg::ResultSet> result, const std::shared_ptr<Counts> &counts)
{
  if (!result) {
    static_cast<void>(weave::report_error(result.error()));
    std::abort();
  }
  check(counts->closed == 1 && counts->created + counts->copied == counts->destroyed + 1);
  {
    auto copy = *result;
    check(copy.rows.size() == 7);
  }
  check(counts->created + counts->copied == counts->destroyed + 1);
}

static void blocking(pg::Options options)
{
  auto counts = std::make_shared<Counts>();
  auto connection = pg::BlockingConnection::connect(options);
  check(bool(connection));
  auto key = connection->on_event("audit", observer(counts));
  check(bool(key));
  auto results = connection->query("SELECT 1; SELECT 2");
  check(results && results->size() == 2 && results->front().event_data(*key));
  copies(results->front(), *key, counts);
  check(bool(connection->reset(options)) && counts->reset == 1);
  check(bool(connection->finish()));
}

int main()
{
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
    auto counts = std::make_shared<Counts>();
    verify(ctx->run(weave::timeout(15s, observe(profile, counts))), counts);
    blocking(profile);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  constexpr unsigned workers = 4;
  constexpr unsigned roots = 16;
  constexpr unsigned scheduler_count = 2;
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(bool(runtime));
    std::vector<weave::JoinHandle<pg::ResultSet>> jobs;
    std::vector<std::shared_ptr<Counts>> counters;
    for (unsigned index = 0; index < 16; ++index) {
      auto counts = std::make_shared<Counts>();
      auto job = runtime->spawn(weave::timeout(15s, observe(profiles[index % profiles.size()], counts)));
      check(bool(job));
      jobs.push_back(std::move(*job));
      counters.push_back(std::move(counts));
    }
    for (std::size_t index = 0; index < jobs.size(); ++index)
      verify(std::move(jobs[index]).get(), counters[index]);
  }
#else
  constexpr unsigned workers = 0;
  constexpr unsigned roots = 0;
  constexpr unsigned scheduler_count = 0;
#endif
  std::printf(
    "Selective-copy live controls passed: %u checks; workers=%u roots=%u schedulers=%u profiles=plain,mtls\n",
    checks.load(),
    workers,
    roots,
    scheduler_count);
}
