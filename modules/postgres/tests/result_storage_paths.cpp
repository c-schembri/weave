#include "result_storage_inspection.hpp"
#include <weave/io.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/timer.hpp>
#include <weave/log.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#include <openssl/crypto.h>
#include <array>
#include <iostream>
#if defined(_WIN32)
#include <crtdbg.h>
#endif

namespace pg = weave::pg;
namespace storage = weave::pg::detail;
using memory_test::check;
using namespace std::chrono_literals;

static std::atomic<unsigned> row_streams{0};
static std::atomic<unsigned> replication_streams{0};

static constexpr std::string_view
  row_query = "SELECT g::text AS ordinal, repeat('x',100) AS payload, NULL::text AS missing, ''::text AS empty "
              "FROM generate_series(1,128) AS g";

struct Rows {
  std::vector<pg::Row> values;
  std::vector<storage::ResultArena> arenas;
  std::optional<storage::ResultText> column;

  void accept(pg::Row row)
  {
    memory_test::own_row(row);
    const auto arena = row.get_allocator().arena();
    for (const auto &previous : arenas)
      check(previous.identity() != arena.identity());
    arenas.push_back(arena);
    values.push_back(std::move(row));
  }

  void verify()
  {
    check(values.size() == 128 && arenas.size() == values.size());
    check(column && *column == "payload");
    for (std::size_t index = 0; index < values.size(); ++index) {
      const auto &row = values[index];
      check(row.size() == 4);
      check(row[0].bytes() == std::to_string(index + 1));
      check(row[1].bytes() == std::string(100, 'x'));
      check(row[2].is_null() && !row[3].is_null() && row[3].bytes().empty());
      check(row.get_allocator().arena().identity() == arenas[index].identity());
      pg::ResultSet result;
      result.rows.push_back(row);
      memory_test::inspect(result);
    }

    auto text = std::move(*values.front()[1].data);
    const auto arena = text.get_allocator().arena().identity();
    values.clear();
    arenas.clear();
    check(text == std::string(100, 'x'));
    check(text.get_allocator().arena().identity() == arena);
    row_streams.fetch_add(1, std::memory_order_relaxed);
  }
};

static weave::Task<void> row_session(pg::Options options)
{
  Rows retained;
  {
    auto connection = co_await pg::connect(options);
    co_await connection.start_rows(std::string(row_query));
    while (auto row = co_await connection.read_row()) {
      if (!retained.column)
        retained.column = connection.row_columns()[1].name;
      retained.accept(std::move(*row));
    }
    check(connection.open());
    auto recovered = co_await connection.query("SELECT 1");
    check(recovered.size() == 1);
    memory_test::inspect(recovered.front());
    co_await connection.reset(options);
    co_await connection.finish();
  }
  retained.verify();
}

static void row_blocking(pg::Options options)
{
  Rows retained;
  {
    auto connection = pg::BlockingConnection::connect(options);
    check(connection.has_value());
    check(connection->start_rows(std::string(row_query)).has_value());
    for (;;) {
      auto next = connection->read_row();
      check(next.has_value());
      if (!*next)
        break;
      if (!retained.column)
        retained.column = connection->row_columns()[1].name;
      retained.accept(std::move(**next));
    }
    check(connection->open());
    auto recovered = connection->query("SELECT 1");
    check(recovered && recovered->size() == 1);
    memory_test::inspect(recovered->front());
    check(connection->reset(options).has_value());
    check(connection->finish().has_value());
  }
  retained.verify();
}

static std::array<std::byte, 34> feedback()
{
  std::array<std::byte, 34> message{};
  message.front() = std::byte{'r'};
  message.back() = std::byte{1};
  return message;
}

static void identity(const pg::Results &results)
{
  check(results.size() == 1);
  const auto &result = results.front();
  check(result.rows.size() == 1 && result.rows.front().size() == 4);
  check(!result.rows.front()[2].is_null());
  memory_test::inspect(result);
}

static void completion(const pg::Results &results, const pg::ResultSet &last)
{
  check(results.size() == 2);
  check(results.front().command == "START_STREAMING");
  check(results.back().command == "START_REPLICATION");
  check(last.command == "START_REPLICATION");
  for (const auto &result : results) {
    memory_test::inspect(result);
    check(result.kind == pg::ResultKind::command && result.rows.empty());
  }
  memory_test::inspect(last);
}

static weave::Task<void> replication_session(pg::Options options)
{
  options.user = "weave_replication";
  options.replication = pg::Replication::physical;
  options.target_session = pg::TargetSession::any;
  pg::Results retained;
  pg::ResultSet last;
  {
    auto connection = co_await pg::connect(options);
    auto system = co_await connection.query("IDENTIFY_SYSTEM");
    identity(system);
    auto lsn = std::string(system.front().rows.front()[2].bytes());
    auto format = co_await connection.start_copy("START_REPLICATION PHYSICAL " + lsn);
    check(format.direction == pg::CopyDirection::both);
    auto message = feedback();
    co_await connection.write_copy(message);
    auto received = co_await connection.read_copy();
    check(received && !received->empty());
    check(received->front() == std::byte{'k'} || received->front() == std::byte{'w'});
    co_await connection.finish_copy_send();
    while (co_await connection.read_copy()) {
    }
    last = co_await connection.end_copy();
    auto results = connection.copy_results();
    check(results.has_value());
    retained = std::move(*results);
    completion(retained, last);
    identity(co_await connection.query("IDENTIFY_SYSTEM"));
    co_await connection.reset(options);
    co_await connection.finish();
  }
  completion(retained, last);
  auto tag = std::move(retained.back().command);
  retained.clear();
  last = pg::ResultSet{};
  check(tag == "START_REPLICATION");
  replication_streams.fetch_add(1, std::memory_order_relaxed);
}

static void replication_blocking(pg::Options options)
{
  options.user = "weave_replication";
  options.replication = pg::Replication::physical;
  options.target_session = pg::TargetSession::any;
  pg::Results retained;
  pg::ResultSet last;
  {
    auto connection = pg::BlockingConnection::connect(options);
    check(connection.has_value());
    auto system = connection->query("IDENTIFY_SYSTEM");
    check(system.has_value());
    identity(*system);
    auto lsn = std::string(system->front().rows.front()[2].bytes());
    auto format = connection->start_copy("START_REPLICATION PHYSICAL " + lsn);
    check(format && format->direction == pg::CopyDirection::both);
    auto message = feedback();
    check(connection->write_copy(message).has_value());
    auto received = connection->read_copy();
    check(received && *received && !(**received).empty());
    check(connection->finish_copy_send().has_value());
    auto finished = connection->end_copy();
    check(finished.has_value());
    last = std::move(*finished);
    auto results = connection->copy_results();
    check(results.has_value());
    retained = std::move(*results);
    completion(retained, last);
    auto reusable = connection->query("IDENTIFY_SYSTEM");
    check(reusable.has_value());
    identity(*reusable);
    check(connection->reset(options).has_value());
    check(connection->finish().has_value());
  }
  completion(retained, last);
  auto tag = std::move(retained.back().command);
  retained.clear();
  last = pg::ResultSet{};
  check(tag == "START_REPLICATION");
  replication_streams.fetch_add(1, std::memory_order_relaxed);
}

static weave::Task<void> session(pg::Options options)
{
  co_await row_session(options);
  co_await replication_session(std::move(options));
}

int main()
{
#if defined(_WIN32)
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  check(OpenSSL_version_num() == OPENSSL_VERSION_NUMBER);
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
    row_blocking(profile);
    replication_blocking(profile);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
#if defined(_WIN32)
  const std::array layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
#else
  const std::array layouts{weave::IoLayout::sharded};
#endif
  for (auto scheduler : schedulers) {
    for (auto layout : layouts) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      check(runtime.has_value());
      std::vector<weave::JoinHandle<void>> jobs;
      for (unsigned index = 0; index < 8; ++index) {
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
  }
#endif
  std::printf(
    "Producer scope: %u row streams, %u replication streams\n",
    row_streams.load(),
    replication_streams.load());
  std::printf(
    "Rows and replication memory: %u inspections, %u checks\n",
    memory_test::inspections.load(),
    memory_test::checks.load());
}
