#include <weave/postgres.hpp>
#ifdef WEAVE_TEST_RUNTIME
#include <weave/runtime.hpp>
#endif
#include <weave/log.hpp>
#include <weave/port.hpp>
#include <weave/timer.hpp>
#include "tls_certificates.hpp"
#include <array>
#include <cstdio>
#include <iostream>

namespace pg = weave::pg;
using namespace std::chrono_literals;

static weave::Task<void> check(bool condition)
{
  if (!condition)
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> mixed(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto flow = co_await connection.exchange(
    "SELECT 1; COPY (SELECT 2) TO STDOUT; SELECT 3; COPY (SELECT 4) TO STDOUT; SELECT 5");
  auto unfinished = flow.finish();
  co_await check(!unfinished && unfinished.error() == pg::Error::busy);
  auto blocked = co_await weave::as_result(connection.query("SELECT 0"));
  co_await check(!blocked && blocked.error() == pg::Error::busy && connection.open());
  auto deferred = flow.next();
  auto moved = std::move(flow);
  auto first_event = co_await std::move(deferred);
  co_await check(first_event && std::holds_alternative<pg::ResultSet>(*first_event));
  std::size_t commands = 1;
  std::size_t copies = 0;
  std::size_t ends = 0;
  std::string bytes;
  while (auto event = co_await moved.next()) {
    if (std::holds_alternative<pg::ResultSet>(*event)) {
      ++commands;
    } else if (auto format = std::get_if<pg::CopyFormat>(&*event)) {
      co_await check(format->direction == pg::CopyDirection::output);
      ++copies;
    } else if (auto data = std::get_if<std::vector<std::byte>>(&*event)) {
      bytes.append(reinterpret_cast<const char *>(data->data()), data->size());
    } else {
      ++ends;
    }
  }
  co_await check(commands == 5 && copies == 2 && ends == 2 && bytes == "2\n4\n");
  auto idle = moved.next();
  auto held = moved.finish();
  co_await check(!held && held.error() == pg::Error::busy);
  co_await check(!(co_await std::move(idle)));
  auto complete = moved.finish();
  if (!complete)
    co_await weave::fail(complete.error());
  co_await connection.query("SELECT 42");

  auto input = co_await connection.exchange(
    "CREATE TEMP TABLE probe(value INT); COPY probe FROM STDIN; SELECT value FROM probe; "
    "COPY probe FROM STDIN; SELECT value FROM probe ORDER BY value");
  commands = 0;
  copies = 0;
  std::optional<weave::Task<void>> stale;
  const std::string first = "42\n";
  const std::string second = "84\n";
  auto first_bytes = std::as_bytes(std::span{first});
  auto second_bytes = std::as_bytes(std::span{second});
  while (auto event = co_await input.next()) {
    if (std::holds_alternative<pg::ResultSet>(*event)) {
      ++commands;
    } else if (auto format = std::get_if<pg::CopyFormat>(&*event)) {
      co_await check(format->direction == pg::CopyDirection::input);
      if (copies == 0) {
        stale.emplace(input.write(first_bytes));
        co_await input.write(first_bytes);
      } else {
        auto rejected = co_await weave::as_result(std::move(*stale));
        stale.reset();
        co_await check(!rejected && rejected.error() == std::errc::invalid_argument);
        co_await input.write(second_bytes);
      }
      co_await input.finish_send();
      ++copies;
    } else {
      co_await check(false);
    }
  }
  co_await check(commands == 5 && copies == 2);
  if (auto status = input.finish(); !status)
    co_await weave::fail(status.error());

  auto errors = co_await connection.exchange("SELECT 1; SELECT no_such_column");
  co_await check((co_await errors.next()).has_value());
  auto failed = co_await weave::as_result(errors.next());
  co_await check(!failed && pg::sqlstate(failed.error()) == "42703" && connection.open());
  if (auto status = errors.finish(); !status)
    co_await weave::fail(status.error());
  co_await connection.query("SELECT 42");
  co_await connection.finish();
}

static weave::Task<void> backup(pg::Options options)
{
  options.user = "weave_replication";
  options.replication = pg::Replication::physical;
  auto connection = co_await pg::connect(options);
  auto flow = co_await connection.exchange("BASE_BACKUP (CHECKPOINT 'fast', MANIFEST 'yes')");
  std::size_t results = 0;
  std::size_t copies = 0;
  std::size_t ends = 0;
  std::size_t bytes = 0;
  bool archive = false;
  bool manifest = false;
  while (auto event = co_await flow.next()) {
    if (std::holds_alternative<pg::ResultSet>(*event)) {
      ++results;
    } else if (auto format = std::get_if<pg::CopyFormat>(&*event)) {
      co_await check(format->direction == pg::CopyDirection::output);
      ++copies;
    } else if (auto data = std::get_if<std::vector<std::byte>>(&*event)) {
      co_await check(!data->empty());
      archive = archive || data->front() == std::byte{'n'};
      manifest = manifest || data->front() == std::byte{'m'};
      bytes += data->size();
    } else {
      ++ends;
    }
  }
  co_await check(results >= 3 && copies == 1 && ends == 1 && bytes > 1024 && archive && manifest);
  if (auto status = flow.finish(); !status)
    co_await weave::fail(status.error());
  co_await connection.query("IDENTIFY_SYSTEM");
  co_await connection.finish();
}

static weave::Task<void> copy_lifetime(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  co_await connection.query("CREATE TEMP TABLE copy_lifetime(value INT)");
  co_await connection.start_copy("COPY copy_lifetime FROM STDIN");
  const std::string text = "42\n";
  auto bytes = std::as_bytes(std::span{text});
  auto delayed = connection.write_copy(bytes);
  auto delayed_end = connection.end_copy();
  co_await connection.write_copy(bytes);
  co_await connection.end_copy();

  co_await connection.start_copy("COPY copy_lifetime FROM STDIN");
  auto rejected = co_await weave::as_result(std::move(delayed));
  co_await check(!rejected && rejected.error() == std::errc::invalid_argument);
  auto rejected_end = co_await weave::as_result(std::move(delayed_end));
  co_await check(!rejected_end && rejected_end.error() == std::errc::invalid_argument);
  co_await connection.write_copy(bytes);
  co_await connection.end_copy();
  auto rows = co_await connection.query("SELECT value FROM copy_lifetime");
  co_await check(rows.front().rows.size() == 2);
  co_await connection.start_copy("COPY (SELECT 1) TO STDOUT");
  auto delayed_read = connection.read_copy();
  while (co_await connection.read_copy()) {
  }
  co_await connection.start_copy("COPY (SELECT 2) TO STDOUT");
  auto rejected_read = co_await weave::as_result(std::move(delayed_read));
  co_await check(!rejected_read && rejected_read.error() == std::errc::invalid_argument);
  std::string received;
  while (auto data = co_await connection.read_copy())
    received.append(reinterpret_cast<const char *>(data->data()), data->size());
  co_await check(received == "2\n");
  co_await connection.finish();
}

static weave::Result<void> blocking(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(options);
  if (!connection)
    return std::unexpected(connection.error());
  auto flow = connection->exchange(
    "CREATE TEMP TABLE blocking_probe(value INT); COPY blocking_probe FROM STDIN; "
    "SELECT value FROM blocking_probe; COPY blocking_probe TO STDOUT");
  if (!flow)
    return std::unexpected(flow.error());

  const std::string input = "42\n84\n";
  std::string output;
  std::size_t commands = 0;
  std::size_t copies = 0;
  std::size_t ends = 0;
  for (;;) {
    auto event = flow->next();
    if (!event)
      return std::unexpected(event.error());
    if (!*event)
      break;
    if (std::holds_alternative<pg::ResultSet>(**event)) {
      ++commands;
    } else if (auto format = std::get_if<pg::CopyFormat>(&**event)) {
      ++copies;
      if (format->direction == pg::CopyDirection::input) {
        if (auto status = flow->write(std::as_bytes(std::span{input})); !status)
          return status;
        if (auto status = flow->finish_send(); !status)
          return status;
      }
    } else if (auto data = std::get_if<std::vector<std::byte>>(&**event)) {
      output.append(reinterpret_cast<const char *>(data->data()), data->size());
    } else {
      ++ends;
    }
  }
  if (commands != 4 || copies != 2 || ends != 1 || output != input)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto status = flow->finish(); !status)
    return status;
  if (auto result = connection->query("SELECT 42"); !result)
    return std::unexpected(result.error());
  auto errors = connection->exchange("SELECT no_such_column");
  if (!errors)
    return std::unexpected(errors.error());
  auto failed = errors->next();
  if (failed || pg::sqlstate(failed.error()) != "42703" || !connection->open())
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto status = errors->finish(); !status)
    return status;
  return connection->finish();
}

static weave::Result<void> blocking_backup(pg::Options options)
{
  options.user = "weave_replication";
  options.replication = pg::Replication::physical;
  auto connection = pg::BlockingConnection::connect(options);
  if (!connection)
    return std::unexpected(connection.error());
  auto flow = connection->exchange("BASE_BACKUP (CHECKPOINT 'fast', MANIFEST 'yes')");
  if (!flow)
    return std::unexpected(flow.error());

  std::size_t results = 0;
  std::size_t copies = 0;
  std::size_t ends = 0;
  std::size_t bytes = 0;
  for (;;) {
    auto event = flow->next();
    if (!event)
      return std::unexpected(event.error());
    if (!*event)
      break;
    if (std::holds_alternative<pg::ResultSet>(**event))
      ++results;
    else if (std::holds_alternative<pg::CopyFormat>(**event))
      ++copies;
    else if (std::holds_alternative<pg::CopyDone>(**event))
      ++ends;
    else
      bytes += std::get<std::vector<std::byte>>(**event).size();
  }
  if (results < 3 || copies != 1 || ends != 1 || bytes < 1024)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto status = flow->finish(); !status)
    return status;
  if (auto result = connection->query("IDENTIFY_SYSTEM"); !result)
    return std::unexpected(result.error());
  return connection->finish();
}

static weave::Task<void> logical(pg::Options options, bool pgoutput, unsigned index, bool legacy = false)
{
  auto writer = co_await pg::connect(options);
  options.user = "weave_replication";
  options.replication = pg::Replication::database;
  auto connection = co_await pg::connect(options);
  auto slot = "probe_slot_" + std::to_string(index);
  auto publication = "probe_pub_" + std::to_string(index);
  auto marker = "payload_" + std::to_string(index);
  if (pgoutput)
    co_await connection.query("CREATE PUBLICATION " + publication + " FOR TABLE public.logical_probe");
  auto plugin = pgoutput ? "pgoutput" : "test_decoding";
  auto created = co_await connection.query(
    "CREATE_REPLICATION_SLOT " + slot + " TEMPORARY LOGICAL " + plugin + " (SNAPSHOT 'nothing')");
  co_await check(created.size() == 1 && created.front().rows.size() == 1 && created.front().rows.front().size() == 4);
  auto lsn = std::string(created.front().rows.front()[1].bytes());
  co_await writer.query("INSERT INTO public.logical_probe VALUES (" + std::to_string(index) + ", '" + marker + "')");
  auto settings = pgoutput ? "(proto_version '1', publication_names '" + publication + "')"
                           : "(\"include-xids\" '0', \"include-timestamp\" '0')";
  auto command = "START_REPLICATION SLOT " + slot + " LOGICAL " + lsn + " " + settings;
  std::optional<pg::Exchange> stream;
  if (legacy) {
    auto stale_finish = connection.finish_copy_send();
    auto format = co_await connection.start_copy(command);
    co_await check(format.direction == pg::CopyDirection::both);
    auto rejected = co_await weave::as_result(std::move(stale_finish));
    co_await check(!rejected && rejected.error() == std::errc::invalid_argument);
  } else {
    stream.emplace(co_await connection.exchange(command));
  }
  std::array<std::byte, 34> feedback{};
  feedback.front() = std::byte{'r'};
  bool inserted = false;
  bool committed = false;
  bool relation = false;
  bool begun = false;
  bool ended = false;
  std::size_t commands = 0;
  std::string text;
  for (;;) {
    std::optional<pg::ExchangeEvent> event;
    if (legacy) {
      auto data = co_await connection.read_copy();
      if (!data)
        break;
      event.emplace(std::move(*data));
    } else {
      event = co_await stream->next();
      if (!event)
        break;
    }
    if (auto format = std::get_if<pg::CopyFormat>(&*event)) {
      co_await check(format->direction == pg::CopyDirection::both);
    } else if (auto data = std::get_if<std::vector<std::byte>>(&*event)) {
      co_await check(!data->empty());
      if (data->front() == std::byte{'k'}) {
        co_await check(data->size() == 18);
        if (data->back() == std::byte{1}) {
          if (legacy)
            co_await connection.write_copy(feedback);
          else
            co_await stream->write(feedback);
        }
      } else {
        co_await check(data->front() == std::byte{'w'} && data->size() > 25);
        auto payload = std::string_view{reinterpret_cast<const char *>(data->data() + 25), data->size() - 25};
        if (pgoutput) {
          begun = begun || (payload.front() == 'B' && payload.size() == 21);
          relation = relation || (payload.front() == 'R' && payload.find("logical_probe") != payload.npos);
          inserted = inserted || (payload.front() == 'I' && payload.find(marker) != payload.npos);
          committed = committed || (inserted && payload.front() == 'C' && payload.size() == 26);
        } else {
          text.append(payload);
          auto insertion = text.find(marker);
          begun = text.find("BEGIN") != text.npos;
          relation = text.find("table public.logical_probe: INSERT:") != text.npos;
          inserted = insertion != text.npos;
          committed = inserted && text.find("COMMIT", insertion) != text.npos;
        }
        if (committed && !ended) {
          ended = true;
          if (legacy)
            co_await connection.finish_copy_send();
          else
            co_await stream->finish_send();
        }
      }
    } else if (std::holds_alternative<pg::ResultSet>(*event)) {
      ++commands;
    }
  }
  if (legacy) {
    co_await connection.end_copy();
    auto results = connection.copy_results();
    if (!results)
      co_await weave::fail(results.error());
    commands = results->size();
  }
  co_await check(begun && relation && inserted && committed && commands == 2);
  if (stream) {
    if (auto status = stream->finish(); !status)
      co_await weave::fail(status.error());
  }
  co_await connection.query("DROP_REPLICATION_SLOT " + slot);
  if (pgoutput)
    co_await connection.query("DROP PUBLICATION " + publication);
  co_await connection.query("SELECT 42");
  co_await connection.finish();
  co_await writer.finish();
}

int main()
{
  fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::array<std::string, 6> arguments;
  for (auto &argument : arguments)
    std::getline(std::cin, argument);
  auto port = weave::parse_port(arguments[0]);
  if (!port)
    return 2;
  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  if (!credentials)
    return weave::report_error(credentials.error());
  auto address = weave::IpAddress::parse(arguments[3]);
  if (!address)
    return weave::report_error(address.error());
  pg::Options options{.host = "localhost", .port = *port, .user = "weave", .password = arguments[1]};
  options.hosts.push_back({.name = "localhost", .port = *port, .address = *address});
  options.database = "postgres";
  options.tls = *credentials;
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());
  if (auto result = ctx->run(weave::timeout(15s, copy_lifetime(options))); !result)
    return weave::report_error(result.error());
  if (auto result = ctx->run(weave::timeout(15s, mixed(options))); !result)
    return weave::report_error(result.error());
  if (auto result = ctx->run(weave::timeout(30s, backup(options))); !result)
    return weave::report_error(result.error());
  bool text_plugin = arguments[5] == "1";
  if (text_plugin) {
    if (auto result = ctx->run(weave::timeout(15s, logical(options, false, 0))); !result)
      return weave::report_error(result.error());
  } else {
    std::fputs("Optional test_decoding plugin unavailable; built-in pgoutput remains mandatory\n", stderr);
  }
  if (auto result = ctx->run(weave::timeout(15s, logical(options, true, 1))); !result)
    return weave::report_error(result.error());
  if (text_plugin) {
    if (auto result = ctx->run(weave::timeout(15s, logical(options, false, 2, true))); !result)
      return weave::report_error(result.error());
  }
  if (auto result = ctx->run(weave::timeout(15s, logical(options, true, 3, true))); !result)
    return weave::report_error(result.error());
  if (auto result = blocking(options); !result)
    return weave::report_error(result.error());
  if (auto result = blocking_backup(options); !result)
    return weave::report_error(result.error());

#ifdef WEAVE_TEST_RUNTIME
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  const std::array plugins{false, true};
#ifdef _WIN32
  const std::array layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
#else
  const std::array layouts{weave::IoLayout::sharded};
#endif
  unsigned index = 10;
  for (auto scheduler : schedulers) {
    for (auto layout : layouts) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      if (!runtime)
        return weave::report_error(runtime.error());
      std::vector<weave::JoinHandle<void>> jobs;
      for (std::size_t client = 0; client < 16; ++client) {
        auto job = runtime->spawn(weave::timeout(20s, mixed(options)));
        if (!job)
          return weave::report_error(job.error());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs) {
        if (auto result = std::move(job).get(); !result)
          return weave::report_error(result.error());
      }
      jobs.clear();
      for (bool pgoutput : plugins) {
        if (!pgoutput && !text_plugin)
          continue;
        auto job = runtime->spawn(weave::timeout(20s, logical(options, pgoutput, index++)));
        if (!job)
          return weave::report_error(job.error());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs) {
        if (auto result = std::move(job).get(); !result)
          return weave::report_error(result.error());
      }
    }
  }
#endif
  std::puts("mixed COPY, blocking, lifetimes, BASE_BACKUP and logical replication passed");
}
