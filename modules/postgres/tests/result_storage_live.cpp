#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#include <algorithm>
#include <format>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <source_location>

namespace pg = weave::pg;
using namespace std::chrono_literals;
static std::atomic<unsigned> checks = 0;
static std::atomic<unsigned> inspections = 0;

static void check(bool condition, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Result live check failed at line %u\n", location.line());
    std::exit(1);
  }
}

static void memory(const pg::ResultSet &result)
{
  ++inspections;
  const auto before = result.memory_size();
  auto copied = result.copy({.observers = false});
  std::vector<const void *> seen;
  std::size_t expected = 0;
  auto add = [&](const pg::detail::ResultArena &arena) {
    const auto identity = arena.identity();
    if (identity && std::find(seen.begin(), seen.end(), identity) == seen.end()) {
      seen.push_back(identity);
      expected += arena.bytes();
    }
  };
  add(copied.columns.get_allocator().arena());
  add(copied.rows.get_allocator().arena());
  add(copied.command.get_allocator().arena());
  add(copied.parameter_types.get_allocator().arena());
  for (const auto &column : copied.columns)
    add(column.name.get_allocator().arena());
  for (const auto &row : copied.rows) {
    add(row.get_allocator().arena());
    for (const auto &value : row) {
      if (value.data)
        add(value.data->get_allocator().arena());
    }
  }
  check(copied.memory_size() == expected);
  check(copied.command == result.command && copied.columns.size() == result.columns.size());
  check(copied.rows.size() == result.rows.size());
  check(result.memory_size() == before);
  check(std::format("{}", result.command) == std::string_view{result.command});
}

static void kind(const pg::ResultSet &result, pg::ResultKind expected)
{
  memory(result);
  check(result.kind == expected);
  auto copy = result;
  check(copy.kind == expected);
  pg::ResultSet assigned;
  check(assigned.kind == pg::ResultKind::uninitialized);
  assigned = copy;
  check(assigned.kind == expected);
  pg::ResultSet moved{std::move(copy)};
  check(moved.kind == expected);
  assigned = std::move(moved);
  check(assigned.kind == expected);
}

static void failure(const pg::Outcome &outcome)
{
  check(!outcome.result && !outcome.aborted);
  check(outcome.error.sqlstate() == "22012" && outcome.error.message() == "division by zero");
  check(outcome.error.field('V') == "ERROR");
}

static weave::Task<void> consume(pg::Pipeline &pipeline)
{
  unsigned completed = 0;
  unsigned rows = 0;
  while (auto event = co_await pipeline.next()) {
    if (!event->complete) {
      check(event->id == 3 && event->outcome.result.has_value());
      kind(*event->outcome.result, pg::ResultKind::row_chunk);
      rows += static_cast<unsigned>(event->outcome.result->rows.size());
      continue;
    }
    check(event->id == ++completed && !event->outcome.aborted && event->outcome.error.fields.empty());
    if (event->id == 7) {
      check(!event->outcome.result && event->transaction == pg::Transaction::idle);
      continue;
    }
    const auto &result = *event->outcome.result;
    switch (event->id) {
    case 1:
    case 6:
      kind(result, pg::ResultKind::acknowledgment);
      break;
    case 2:
      kind(result, pg::ResultKind::description);
      break;
    case 3:
    case 4:
      kind(result, pg::ResultKind::tuples);
      check(result.rows.empty());
      break;
    case 5:
      kind(result, pg::ResultKind::command);
      break;
    default:
      check(false);
    }
  }
  check(completed == 7 && rows == 5);
}

static weave::Task<void> session(pg::Options options)
{
  unsigned creates = 0;
  auto connection = co_await pg::connect(options);
  auto observer = connection.on_event("kind", [&](pg::Event &event) noexcept -> weave::Result<void> {
    if (event.kind == pg::EventKind::result_create || event.kind == pg::EventKind::result_copy)
      check(event.result && event.result->kind != pg::ResultKind::uninitialized);
    if (event.kind == pg::EventKind::result_create)
      ++creates;
    return {};
  });
  check(observer.has_value());
  {
    auto deferred = connection.query_outcomes("must_not_send");
    auto blocked = co_await weave::as_result(connection.reset(options));
    check(!blocked && blocked.error() == pg::Error::busy && connection.open());
  }
  {
    auto deferred = connection.execute_outcome("must_not_send");
    check(!connection.info());
  }
  {
    auto deferred = connection.execute_prepared_outcome("must_not_send");
    check(!connection.info());
  }

  auto results = co_await connection.query_outcomes(
    "SET application_name TO 'outcomes'; SELECT 1 WHERE false; SELECT FROM generate_series(1,2); SELECT 1/0; SELECT "
    "99");
  check(results.size() == 4);
  check(creates == 3);
  kind(*results[0].result, pg::ResultKind::command);
  kind(*results[1].result, pg::ResultKind::tuples);
  check(results[1].result->rows.empty() && results[1].result->columns.size() == 1);
  kind(*results[2].result, pg::ResultKind::tuples);
  check(results[2].result->columns.empty() && results[2].result->rows.size() == 2);
  failure(results[3]);
  auto retained = results[3];
  auto empty = co_await connection.query_outcomes(" ; ");
  check(empty.size() == 1 && empty[0].result.has_value());
  kind(*empty[0].result, pg::ResultKind::empty_query);
  check(connection.last_error().fields.empty());
  failure(retained);
  auto legacy = co_await weave::as_result(connection.query("SELECT 1/0"));
  check(!legacy && pg::sqlstate(legacy.error()) == "22012" && connection.open());

  auto before_failure = creates;
  auto failed = co_await connection.execute_outcome("SELECT 1/$1::int", {{"0", 23}});
  failure(failed);
  check(creates == before_failure);
  auto zero = co_await connection.execute_outcome("SELECT 1 WHERE false");
  check(zero.result.has_value());
  kind(*zero.result, pg::ResultKind::tuples);
  auto utility = co_await connection.execute_outcome("SET application_name TO 'result_kind'");
  kind(*utility.result, pg::ResultKind::command);
  auto extended_empty = co_await connection.execute_outcome("");
  kind(*extended_empty.result, pg::ResultKind::empty_query);
  auto binary = co_await connection.execute_outcome("SELECT 7::int", {}, pg::Format::binary);
  kind(*binary.result, pg::ResultKind::tuples);
  check(binary.result->rows[0][0].binary_integer<int>() == weave::Result<int>{7});

  co_await connection.prepare("probe", "SELECT 1/$1::int", {23});
  failure(co_await connection.execute_prepared_outcome("probe", {{"0", 23}}));
  auto prepared = co_await connection.execute_prepared_outcome("probe", {{"1", 23}});
  kind(*prepared.result, pg::ResultKind::tuples);
  auto description = co_await connection.describe("probe");
  kind(description, pg::ResultKind::description);
  co_await connection.close_prepared("probe");

  co_await connection.query("BEGIN");
  failure(co_await connection.execute_outcome("SELECT 1/0"));
  check(connection.transaction() == pg::Transaction::failed);
  auto aborted = co_await connection.execute_outcome("SELECT 1");
  check(!aborted.result && !aborted.aborted && aborted.error.sqlstate() == "25P02");
  co_await connection.query("ROLLBACK");
  co_await connection.query("BEGIN");
  auto opened = co_await connection.open_portal("cursor", "SELECT generate_series(1,2)");
  kind(opened, pg::ResultKind::description);
  kind(co_await connection.describe_portal("cursor"), pg::ResultKind::description);
  auto first = co_await connection.fetch("cursor", 1);
  kind(first, pg::ResultKind::row_chunk);
  check(first.suspended);
  auto last = co_await connection.fetch("cursor", 8);
  kind(last, pg::ResultKind::tuples);
  check(!last.suspended);
  co_await connection.close_portal("cursor");
  auto no_data = co_await connection.open_portal("utility", "SET application_name TO 'no_data'");
  kind(no_data, pg::ResultKind::description);
  co_await connection.close_portal("utility");
  co_await connection.query("COMMIT");

  std::vector<pg::Command> commands{{"SELECT 1 WHERE false"}, {"SET application_name TO 'batch'"}, {""}};
  auto batch = co_await connection.batch(std::move(commands));
  check(batch.size() == 3);
  kind(*batch[0].result, pg::ResultKind::tuples);
  kind(*batch[1].result, pg::ResultKind::command);
  kind(*batch[2].result, pg::ResultKind::empty_query);
  {
    auto pipeline = connection.pipeline();
    check(pipeline.has_value());
    check(pipeline->prepare("chunk", "SELECT generate_series(1,5)").has_value());
    check(pipeline->describe("chunk").has_value());
    check(pipeline->execute_prepared("chunk", {}, pg::Format::text, {.chunk_rows = 2}).has_value());
    check(pipeline->execute({"SELECT 1 WHERE false"}, {.chunk_rows = 2}).has_value());
    check(pipeline->execute({"SET application_name TO 'pipeline'"}).has_value());
    check(pipeline->close_prepared("chunk").has_value());
    check(pipeline->sync().has_value());
    co_await weave::when_all(pipeline->flush(), consume(*pipeline));
    check(pipeline->finish().has_value());
  }
  co_await connection.start_copy("COPY (SELECT 1) TO STDOUT");
  while (co_await connection.read_copy()) {
  }
  auto copied = connection.copy_result();
  check(copied.has_value());
  kind(*copied, pg::ResultKind::command);
  {
    auto exchange = co_await connection.exchange("SET application_name TO 'exchange'; SELECT 1 WHERE false");
    auto command = co_await exchange.next();
    check(command.has_value());
    kind(std::get<pg::ResultSet>(*command), pg::ResultKind::command);
    auto tuples = co_await exchange.next();
    check(tuples.has_value());
    kind(std::get<pg::ResultSet>(*tuples), pg::ResultKind::tuples);
    check(!(co_await exchange.next()));
    check(exchange.finish().has_value());
  }
  co_await connection.reset(options);
  failure(retained);
  co_await connection.finish();
  failure(retained);
  kind(description, pg::ResultKind::description);
  auto closed = co_await weave::as_result(connection.query_outcomes("SELECT 1"));
  check(!closed && closed.error() == pg::Error::closed);
}

static void blocking(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection.has_value());
  auto result = connection->query_outcomes("SELECT 1; SELECT 1/0; SELECT 99");
  check(result && result->size() == 2);
  kind(*result->front().result, pg::ResultKind::tuples);
  failure(result->back());
  auto failed = connection->execute_outcome("SELECT 1/0");
  check(failed.has_value());
  failure(*failed);
  check(connection->prepare("blocking", "SELECT 1/$1::int", {23}).has_value());
  auto prepared = connection->execute_prepared_outcome("blocking", {{"0", 23}});
  check(prepared.has_value());
  failure(*prepared);
  auto reusable = connection->execute_outcome("SELECT 1 WHERE false");
  check(reusable && reusable->result.has_value());
  kind(*reusable->result, pg::ResultKind::tuples);
  check(connection->reset(options).has_value());
  failure(result->back());
  check(connection->finish().has_value());
  failure(result->back());
}

int main()
{
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
  const std::array names{
    std::pair{pg::ResultKind::uninitialized, "uninitialized"},
    std::pair{pg::ResultKind::empty_query, "empty_query"},
    std::pair{pg::ResultKind::command, "command"},
    std::pair{pg::ResultKind::tuples, "tuples"},
    std::pair{pg::ResultKind::description, "description"},
    std::pair{pg::ResultKind::row_chunk, "row_chunk"},
    std::pair{pg::ResultKind::acknowledgment, "acknowledgment"}};
  for (const auto &[value, name] : names)
    check(pg::result_kind_name(value) == name);
  check(pg::result_kind_name(static_cast<pg::ResultKind>(999)) == "unknown");
  static fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::string port_text, password, standby, address_text, local;
  std::getline(std::cin, port_text);
  std::getline(std::cin, password);
  std::getline(std::cin, standby);
  std::getline(std::cin, address_text);
  std::getline(std::cin, local);
  std::printf("Owned certificate fixture: %s\n", certificates.directory.string().c_str());
  auto port = weave::parse_port(port_text);
  auto address = weave::IpAddress::parse(address_text);
  check(port && address);
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  check(credentials.has_value());
  pg::Options plain;
  plain.host = "localhost";
  plain.hosts = {{"localhost", *port, *address}};
  plain.user = "weave";
  plain.database = "postgres";
  plain.password = password;
  plain.plaintext = true;
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = pg::ChannelBinding::require;
  const std::array profiles{plain, secured};
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  for (const auto &profile : profiles) {
    auto result = ctx->run(weave::timeout(30s, session(profile)));
    if (!result)
      return weave::report_error(result.error());
    blocking(profile);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(runtime.has_value());
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 32; ++index) {
      auto job = runtime->spawn(weave::timeout(30s, session(profiles[index % profiles.size()])));
      check(job.has_value());
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      auto result = std::move(job).get();
      if (!result)
        return weave::report_error(result.error());
    }
  }
#endif
  std::printf("Result producer memory controls: %u inspections, %u checks\n", inspections.load(), checks.load());
}
