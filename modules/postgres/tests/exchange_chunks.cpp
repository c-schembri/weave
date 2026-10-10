#include <weave/postgres.hpp>
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
#include <weave/runtime.hpp>
#endif
#include <weave/port.hpp>
#include <weave/timer.hpp>
#include <weave/scope.hpp>
#if defined(WEAVE_POSTGRES_TEST_LIVE)
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
static constexpr std::array<weave::u32, 6> sizes{0, 1, 2, 3, 5, 7};
static constexpr std::string_view
  select_five = "SELECT i AS n, repeat('x',120) AS payload, CASE WHEN i%2=0 THEN NULL ELSE '' END AS nullable "
                "FROM generate_series(1,5) i";
static constexpr std::string_view
  select_zero = "SELECT i AS n, repeat('x',120) AS payload, CASE WHEN i%2=0 THEN NULL ELSE '' END AS nullable "
                "FROM generate_series(1,0) i";
static constexpr std::string_view
  select_error = "SELECT i AS n, repeat('x',120) AS payload, CASE WHEN i=4 THEN (1/(i-4))::text "
                 "ELSE CASE WHEN i%2=0 THEN NULL ELSE '' END END AS nullable FROM generate_series(1,4) i";

template <class T>
static void check(const T &value, std::source_location location = std::source_location::current())
{
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Exchange chunks check failed: %u\n", location.line());
    std::exit(1);
  }
}

static void schema(const pg::ResultSet &result)
{
  check(result.columns.size() == 3);
  check(result.columns[0].name == "n" && result.columns[0].type == 23);
  check(result.columns[1].name == "payload" && result.columns[1].type == 25);
  check(result.columns[2].name == "nullable" && result.columns[2].type == 25);
  for (const auto &column : result.columns)
    check(column.format == pg::Format::text && column.table == 0 && column.attribute == 0 && column.modifier == -1);
  check(result.columns[0].type_size == 4 && result.columns[1].type_size == -1 && result.columns[2].type_size == -1);
}

static void rows(const pg::ResultSet &result, unsigned &next, unsigned payload = 120)
{
  schema(result);
  for (const auto &row : result.rows) {
    check(row.size() == 3 && row[0].data && row[1].data);
    for (const auto &value : row)
      check(value.format == pg::Format::text);
    check(*row[0].data == std::to_string(next));
    check(*row[1].data == std::string(payload, 'x'));
    check(row[2].data.has_value() == (next % 2 != 0));
    if (row[2].data)
      check(row[2].data->empty());
    ++next;
  }
}

static void result(
  const pg::ResultSet &value,
  weave::u32 size,
  unsigned &next,
  bool &complete,
  unsigned count = 5,
  unsigned payload = 120)
{
  check(!complete);
  if (value.kind == pg::ResultKind::row_chunk) {
    check(size && !value.rows.empty() && value.rows.size() <= size && value.command.empty());
  } else {
    check(value.kind == pg::ResultKind::tuples && value.command == "SELECT " + std::to_string(count));
    check(!size || value.rows.empty());
    complete = true;
  }
  rows(value, next, payload);
}

static void zero_columns(const pg::ResultSet &value, unsigned &count, bool &complete)
{
  check(!complete && value.columns.empty());
  if (value.kind == pg::ResultKind::row_chunk) {
    check(!value.rows.empty() && value.rows.size() <= 2 && value.command.empty());
  } else {
    check(value.kind == pg::ResultKind::tuples && value.rows.empty() && value.command == "SELECT 5");
    complete = true;
  }
  for (const auto &row : value.rows) {
    check(row.empty());
    ++count;
  }
}

static void binary(const pg::ResultSet &value, unsigned &next, bool &complete)
{
  check(!complete && value.columns.size() == 3);
  const auto &number = value.columns[0];
  check(number.name == "n" && number.type == 23 && number.type_size == 4 && number.modifier == -1);
  check(number.table == 411 && number.attribute == 2 && number.format == pg::Format::binary);
  const auto &payload = value.columns[1];
  check(payload.name == "payload" && payload.type == 17 && payload.type_size == -1 && payload.modifier == -1);
  check(payload.table == 411 && payload.attribute == 3 && payload.format == pg::Format::binary);
  const auto &text = value.columns[2];
  check(text.name == "caf\xc3\xa9" && text.type == 25 && text.type_size == -1 && text.modifier == 7);
  check(text.table == 0 && text.attribute == 0 && text.format == pg::Format::text);
  if (value.kind == pg::ResultKind::row_chunk) {
    check(!value.rows.empty() && value.rows.size() <= 2 && value.command.empty());
  } else {
    check(value.kind == pg::ResultKind::tuples && value.rows.empty() && value.command == "SELECT 5");
    complete = true;
  }
  for (const auto &row : value.rows) {
    check(row.size() == 3);
    check(row[0].format == pg::Format::binary && row[0].binary_integer<weave::i32>() == next);
    check(row[1].format == pg::Format::binary && row[1].data && row[1].bytes() == std::string_view{"a\0b\xff", 4});
    check(row[2].format == pg::Format::text && row[2].data.has_value() == (next % 2 != 0));
    if (row[2].data)
      check(row[2].bytes() == "caf\xc3\xa9");
    ++next;
  }
}

static weave::Task<void> session(pg::Options options, bool live = false)
{
  auto connection = co_await pg::connect(options);
  auto observed = std::make_shared<std::array<std::atomic<unsigned>, 2>>();
  check(connection.on_event("chunks", [observed](pg::Event &event) noexcept -> weave::Result<void> {
    if (event.kind == pg::EventKind::result_create) {
      check(event.result && (event.result->kind != pg::ResultKind::row_chunk || !event.result->rows.empty()));
      ++(*observed)[0];
    } else if (event.kind == pg::EventKind::result_destroy) {
      ++(*observed)[1];
    }
    return {};
  }));

  std::vector<pg::ResultSet> retained;
  for (auto size : sizes) {
    auto exchange = co_await connection.exchange(live ? std::string(select_five) : "FIVE", {.chunk_rows = size});
    check(!exchange.finish());
    {
      auto deferred = exchange.next();
      check(!exchange.finish());
    }
    auto moved = std::move(exchange);
    unsigned next = 1;
    bool complete = false;
    unsigned chunks = 0;
    while (auto event = co_await moved.next()) {
      check(std::holds_alternative<pg::ResultSet>(*event));
      auto value = std::get<pg::ResultSet>(std::move(*event));
      result(value, size, next, complete);
      if (value.kind == pg::ResultKind::row_chunk)
        ++chunks;
      retained.push_back(std::move(value));
    }
    check(complete && next == 6 && chunks == (size ? (5 + size - 1) / size : 0));
    check(moved.finish());
    check(connection.open() && connection.transaction() == pg::Transaction::idle);
  }

  {
    auto exchange = co_await connection.exchange(live ? std::string(select_zero) : "ZERO", {.chunk_rows = 2});
    auto event = co_await exchange.next();
    check(event && std::holds_alternative<pg::ResultSet>(*event));
    auto &value = std::get<pg::ResultSet>(*event);
    check(value.kind == pg::ResultKind::tuples && value.rows.empty() && value.command == "SELECT 0");
    schema(value);
    check(!(co_await exchange.next()) && exchange.finish());
  }
  {
    auto sql = live ? std::string(select_five) + "; DO $$ BEGIN END $$; " + std::string(select_zero) : "MULTI";
    auto exchange = co_await connection.exchange(std::move(sql), {.chunk_rows = 2});
    unsigned next = 1;
    bool complete = false;
    while (!complete) {
      auto event = co_await exchange.next();
      check(event && std::holds_alternative<pg::ResultSet>(*event));
      result(std::get<pg::ResultSet>(*event), 2, next, complete);
    }
    check(next == 6);
    auto command = co_await exchange.next();
    check(command && std::holds_alternative<pg::ResultSet>(*command));
    auto &value = std::get<pg::ResultSet>(*command);
    check(value.kind == pg::ResultKind::command && value.columns.empty() && value.rows.empty());
    auto zero = co_await exchange.next();
    check(zero && std::holds_alternative<pg::ResultSet>(*zero));
    auto &empty = std::get<pg::ResultSet>(*zero);
    schema(empty);
    check(empty.kind == pg::ResultKind::tuples && empty.rows.empty() && empty.command == "SELECT 0");
    check(!(co_await exchange.next()) && exchange.finish());
  }
  {
    auto exchange = co_await connection.exchange(live ? std::string(select_error) : "ERROR", {.chunk_rows = 2});
    auto first = co_await exchange.next();
    check(first && std::holds_alternative<pg::ResultSet>(*first));
    auto &value = std::get<pg::ResultSet>(*first);
    unsigned next = 1;
    rows(value, next);
    check(value.kind == pg::ResultKind::row_chunk && next == 3);
    auto failed = co_await weave::as_result(exchange.next());
    check(!failed && pg::sqlstate(failed.error()) == "22012");
    check(exchange.finish() && connection.open());
  }
  {
    auto exchange = co_await connection.exchange("", {.chunk_rows = 1});
    auto event = co_await exchange.next();
    check(event && std::get<pg::ResultSet>(*event).kind == pg::ResultKind::empty_query);
    check(!(co_await exchange.next()) && exchange.finish());
  }
  {
    auto sql = live ? "COPY (SELECT 'hi') TO STDOUT; " + std::string(select_five) : "COPY";
    auto exchange = co_await connection.exchange(std::move(sql), {.chunk_rows = 2});
    auto start = co_await exchange.next();
    check(start && std::holds_alternative<pg::CopyFormat>(*start));
    check(std::get<pg::CopyFormat>(*start).direction == pg::CopyDirection::output);
    std::string bytes;
    for (;;) {
      auto event = co_await exchange.next();
      check(event);
      if (std::holds_alternative<pg::CopyDone>(*event))
        break;
      check(std::holds_alternative<std::vector<std::byte>>(*event));
      auto &data = std::get<std::vector<std::byte>>(*event);
      bytes.append(reinterpret_cast<const char *>(data.data()), data.size());
    }
    check(bytes == "hi\n");
    auto command = co_await exchange.next();
    check(command && std::get<pg::ResultSet>(*command).command == "COPY 1");
    unsigned next = 1;
    bool complete = false;
    while (auto event = co_await exchange.next())
      result(std::get<pg::ResultSet>(*event), 2, next, complete);
    check(complete && next == 6 && exchange.finish());
  }
  {
    auto sql = live ? "SELECT FROM generate_series(1,5)" : "NO_COLUMNS";
    auto exchange = co_await connection.exchange(sql, {.chunk_rows = 2});
    unsigned count = 0;
    bool complete = false;
    while (auto event = co_await exchange.next()) {
      auto &value = std::get<pg::ResultSet>(*event);
      zero_columns(value, count, complete);
      retained.push_back(std::move(value));
    }
    check(count == 5 && complete && exchange.finish());
  }
  if (!live) {
    auto exchange = co_await connection.exchange("BINARY", {.chunk_rows = 2});
    unsigned next = 1;
    bool complete = false;
    while (auto event = co_await exchange.next()) {
      auto &value = std::get<pg::ResultSet>(*event);
      binary(value, next, complete);
      retained.push_back(std::move(value));
    }
    check(next == 6 && complete && exchange.finish());
  }
  {
    auto sql = live ? std::string(select_five) + " LIMIT 2" : "DEFER";
    auto exchange = co_await connection.exchange(std::move(sql), {.chunk_rows = 3});
    auto first = co_await exchange.next();
    check(first && std::holds_alternative<pg::ResultSet>(*first));
    auto &value = std::get<pg::ResultSet>(*first);
    unsigned next = 1;
    bool complete = false;
    result(value, 3, next, complete, 2);
    check(next == 3 && !complete);

    // The partial chunk left CommandComplete pending. Cancellation before the
    // next reader starts must leave that message and the session reusable.
    auto deferred = exchange.next();
    weave::TaskScope tasks;
    tasks.cancel();
    auto job = tasks.spawn(std::move(deferred));
    check(job);
    auto failed = co_await weave::as_result(std::move(*job));
    check(!failed && failed.error() == std::errc::operation_canceled);
    auto joined = co_await weave::as_result(tasks.join());
    check(!joined && joined.error() == std::errc::operation_canceled);
    check(connection.open() && !exchange.finish());

    auto terminal = co_await exchange.next();
    check(terminal && std::holds_alternative<pg::ResultSet>(*terminal));
    result(std::get<pg::ResultSet>(*terminal), 3, next, complete, 2);
    check(next == 3 && complete && !(co_await exchange.next()) && exchange.finish());
    retained.push_back(std::move(value));
  }
  co_await connection.finish();
  retained.clear();
  check((*observed)[0] == (*observed)[1]);

  if (live)
    co_return;
  options.limits.message_bytes = 2048;
  options.limits.result_bytes = 2048;
  auto limited = co_await pg::connect(options);
  {
    auto exchange = co_await limited.exchange("LIMIT", {.chunk_rows = 100});
    unsigned next = 1;
    bool complete = false;
    unsigned chunks = 0;
    while (auto event = co_await exchange.next()) {
      auto &value = std::get<pg::ResultSet>(*event);
      result(value, 100, next, complete, 100, 200);
      if (value.kind == pg::ResultKind::row_chunk)
        ++chunks;
    }
    check(complete && next == 101 && chunks > 1 && exchange.finish());
  }
  co_await limited.finish();
  auto oversized = co_await pg::connect(options);
  {
    auto exchange = co_await oversized.exchange("BIG", {.chunk_rows = 1});
    auto failed = co_await weave::as_result(exchange.next());
    check(!failed && failed.error() == pg::Error::resource_limit);
    check(!oversized.open() && !exchange.finish());
  }
}

static void blocking(pg::Options options, bool live = false)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection);
  for (auto size : sizes) {
    auto exchange = connection->exchange(live ? std::string(select_five) : "FIVE", {.chunk_rows = size});
    check(exchange);
    unsigned next = 1;
    bool complete = false;
    for (;;) {
      auto event = exchange->next();
      check(event);
      if (!*event)
        break;
      result(std::get<pg::ResultSet>(**event), size, next, complete);
    }
    check(next == 6 && complete && exchange->finish());
  }
  {
    auto exchange = connection->exchange(live ? std::string(select_zero) : "ZERO", {.chunk_rows = 2});
    check(exchange);
    auto event = exchange->next();
    check(event && *event);
    auto &value = std::get<pg::ResultSet>(**event);
    schema(value);
    check(value.kind == pg::ResultKind::tuples && value.rows.empty() && value.command == "SELECT 0");
    auto end = exchange->next();
    check(end && !*end && exchange->finish());
  }
  {
    auto sql = live ? std::string(select_five) + "; DO $$ BEGIN END $$; " + std::string(select_zero) : "MULTI";
    auto exchange = connection->exchange(std::move(sql), {.chunk_rows = 2});
    check(exchange);
    unsigned next = 1;
    bool complete = false;
    while (!complete) {
      auto event = exchange->next();
      check(event && *event);
      result(std::get<pg::ResultSet>(**event), 2, next, complete);
    }
    check(next == 6);
    auto command = exchange->next();
    check(command && *command);
    auto &value = std::get<pg::ResultSet>(**command);
    check(
      value.kind == pg::ResultKind::command && value.columns.empty() && value.rows.empty() && value.command == "DO");
    auto zero = exchange->next();
    check(zero && *zero);
    auto &empty = std::get<pg::ResultSet>(**zero);
    schema(empty);
    check(empty.kind == pg::ResultKind::tuples && empty.rows.empty() && empty.command == "SELECT 0");
    auto end = exchange->next();
    check(end && !*end && exchange->finish());
  }
  {
    auto exchange = connection->exchange(live ? std::string(select_error) : "ERROR", {.chunk_rows = 2});
    check(exchange);
    auto first = exchange->next();
    check(first && *first);
    auto &value = std::get<pg::ResultSet>(**first);
    unsigned next = 1;
    rows(value, next);
    check(value.kind == pg::ResultKind::row_chunk && next == 3);
    auto failed = exchange->next();
    check(!failed && pg::sqlstate(failed.error()) == "22012");
    check(exchange->finish() && connection->open());
  }
  {
    auto exchange = connection->exchange("", {.chunk_rows = 1});
    check(exchange);
    auto event = exchange->next();
    check(event && *event && std::get<pg::ResultSet>(**event).kind == pg::ResultKind::empty_query);
    auto end = exchange->next();
    check(end && !*end && exchange->finish());
  }
  {
    auto sql = live ? "COPY (SELECT 'hi') TO STDOUT; " + std::string(select_five) : "COPY";
    auto exchange = connection->exchange(std::move(sql), {.chunk_rows = 2});
    check(exchange);
    auto start = exchange->next();
    check(start && *start && std::holds_alternative<pg::CopyFormat>(**start));
    check(std::get<pg::CopyFormat>(**start).direction == pg::CopyDirection::output);
    std::string bytes;
    for (;;) {
      auto event = exchange->next();
      check(event && *event);
      if (std::holds_alternative<pg::CopyDone>(**event))
        break;
      auto &data = std::get<std::vector<std::byte>>(**event);
      bytes.append(reinterpret_cast<const char *>(data.data()), data.size());
    }
    check(bytes == "hi\n");
    auto command = exchange->next();
    check(command && *command && std::get<pg::ResultSet>(**command).command == "COPY 1");
    unsigned next = 1;
    bool complete = false;
    for (;;) {
      auto event = exchange->next();
      check(event);
      if (!*event)
        break;
      result(std::get<pg::ResultSet>(**event), 2, next, complete);
    }
    check(complete && next == 6 && exchange->finish());
  }
  {
    auto sql = live ? "SELECT FROM generate_series(1,5)" : "NO_COLUMNS";
    auto exchange = connection->exchange(sql, {.chunk_rows = 2});
    check(exchange);
    unsigned count = 0;
    bool complete = false;
    for (;;) {
      auto event = exchange->next();
      check(event);
      if (!*event)
        break;
      zero_columns(std::get<pg::ResultSet>(**event), count, complete);
    }
    check(count == 5 && complete && exchange->finish());
  }
  if (!live) {
    auto exchange = connection->exchange("BINARY", {.chunk_rows = 2});
    check(exchange);
    unsigned next = 1;
    bool complete = false;
    for (;;) {
      auto event = exchange->next();
      check(event);
      if (!*event)
        break;
      binary(std::get<pg::ResultSet>(**event), next, complete);
    }
    check(next == 6 && complete && exchange->finish());
  }
  check(connection->finish());

  if (live)
    return;
  options.limits.message_bytes = 2048;
  options.limits.result_bytes = 2048;
  auto limited = pg::BlockingConnection::connect(options);
  check(limited);
  {
    auto exchange = limited->exchange("LIMIT", {.chunk_rows = 100});
    check(exchange);
    unsigned next = 1;
    bool complete = false;
    unsigned chunks = 0;
    for (;;) {
      auto event = exchange->next();
      check(event);
      if (!*event)
        break;
      auto &value = std::get<pg::ResultSet>(**event);
      result(value, 100, next, complete, 100, 200);
      if (value.kind == pg::ResultKind::row_chunk)
        ++chunks;
    }
    check(next == 101 && complete && chunks > 1 && exchange->finish());
  }
  check(limited->finish());
  auto oversized = pg::BlockingConnection::connect(options);
  check(oversized);
  {
    auto exchange = oversized->exchange("BIG", {.chunk_rows = 1});
    check(exchange);
    auto failed = exchange->next();
    check(!failed && failed.error() == pg::Error::resource_limit);
    check(!oversized->open() && !exchange->finish());
  }
}

static weave::Task<void> early(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto exchange = co_await connection.exchange("EARLY", {.chunk_rows = 2});
  unsigned next = 1;
  bool complete = false;
  auto first = co_await exchange.next();
  check(first && std::holds_alternative<pg::ResultSet>(*first));
  result(std::get<pg::ResultSet>(*first), 2, next, complete);
  check(next == 3 && !complete);
  std::puts("Chunk prefix observed");
  std::fflush(stdout);
  while (auto event = co_await exchange.next())
    result(std::get<pg::ResultSet>(*event), 2, next, complete);
  check(next == 6 && complete && exchange.finish());
  co_await connection.finish();
}

static void blocking_early(pg::Options options)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection);
  auto exchange = connection->exchange("EARLY", {.chunk_rows = 2});
  check(exchange);
  unsigned next = 1;
  bool complete = false;
  auto first = exchange->next();
  check(first && *first && std::holds_alternative<pg::ResultSet>(**first));
  result(std::get<pg::ResultSet>(**first), 2, next, complete);
  check(next == 3 && !complete);
  std::puts("Chunk prefix observed");
  std::fflush(stdout);
  for (;;) {
    auto event = exchange->next();
    check(event);
    if (!*event)
      break;
    result(std::get<pg::ResultSet>(**event), 2, next, complete);
  }
  check(next == 6 && complete && exchange->finish());
  check(connection->finish());
}

static weave::Task<void> terminal(pg::Options options, std::string_view scenario)
{
  auto connection = co_await pg::connect(options);
  weave::TaskScope tasks;
  bool cancellation = scenario == "cancel";
  if (cancellation) {
    check(connection.on_notice([&](const pg::Diagnostic &notice) noexcept {
      check(notice.message() == "PENDING");
      tasks.cancel();
    }));
  }
  const char *sql = "BAD";
  if (cancellation)
    sql = "WAIT";
  else if (scenario == "eof")
    sql = "EOF";
  else if (scenario == "bad_command")
    sql = "BAD_COMMAND";
  auto exchange = co_await connection.exchange(sql, {.chunk_rows = scenario == "bad_command" ? 3u : 2u});
  auto first = co_await exchange.next();
  check(first && std::holds_alternative<pg::ResultSet>(*first));
  auto &value = std::get<pg::ResultSet>(*first);
  unsigned next = 1;
  rows(value, next);
  check(value.kind == pg::ResultKind::row_chunk && next == 3);
  if (cancellation) {
    auto job = tasks.spawn(exchange.next());
    check(job);
    auto failed = co_await weave::as_result(std::move(*job));
    check(!failed && failed.error() == std::errc::operation_canceled);
    auto joined = co_await weave::as_result(tasks.join());
    check(!joined && joined.error() == std::errc::operation_canceled);
  } else {
    auto failed = co_await weave::as_result(exchange.next());
    auto expected = scenario == "eof" ? std::make_error_code(std::errc::connection_reset)
                                      : pg::make_error_code(pg::Error::protocol);
    check(!failed && failed.error() == expected);
  }
  check(!connection.open() && connection.transaction() == pg::Transaction::unknown && !exchange.finish());
  value.rows.front().front().data = "owned after closure";
  check(value.rows.front().front().bytes() == "owned after closure");
}

static void blocking_terminal(pg::Options options, std::string_view scenario)
{
  auto connection = pg::BlockingConnection::connect(options);
  check(connection);
  const char *sql = "BAD";
  if (scenario == "eof")
    sql = "EOF";
  else if (scenario == "bad_command")
    sql = "BAD_COMMAND";
  auto exchange = connection->exchange(sql, {.chunk_rows = scenario == "bad_command" ? 3u : 2u});
  check(exchange);
  auto first = exchange->next();
  check(first && *first && std::holds_alternative<pg::ResultSet>(**first));
  auto &value = std::get<pg::ResultSet>(**first);
  unsigned next = 1;
  rows(value, next);
  check(value.kind == pg::ResultKind::row_chunk && next == 3);
  auto failed = exchange->next();
  auto expected = scenario == "eof" ? std::make_error_code(std::errc::connection_reset)
                                    : pg::make_error_code(pg::Error::protocol);
  check(!failed && failed.error() == expected);
  check(!connection->open() && connection->transaction() == pg::Transaction::unknown && !exchange->finish());
  value.rows.front().front().data = "owned after closure";
  check(value.rows.front().front().bytes() == "owned after closure");
}

static weave::Task<void> scenario_task(pg::Options options, std::string_view scenario)
{
  if (scenario == "normal")
    return session(std::move(options));
  if (scenario == "early")
    return early(std::move(options));
  return terminal(std::move(options), scenario);
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
    check(ctx->run(weave::timeout(15s, session(profile, true))));
    blocking(profile, true);
  }
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    check(runtime);
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto job = runtime->spawn(weave::timeout(15s, session(profiles[index % profiles.size()], true)));
      check(job);
      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs)
      check(std::move(job).get());
  }
  constexpr unsigned workers = 4;
  constexpr unsigned roots = 16;
  constexpr unsigned scheduler_count = 2;
#else
  constexpr unsigned workers = 0;
  constexpr unsigned roots = 0;
  constexpr unsigned scheduler_count = 0;
#endif
  std::printf(
    "Exchange chunks live controls passed: %u checks; workers=%u roots=%u schedulers=%u profiles=plain,mtls\n",
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
  std::string_view key = argv[2];
  auto delimiter = key.find(':');
  auto mode = key.substr(0, delimiter);
  auto scenario = delimiter == key.npos ? std::string_view{"normal"} : key.substr(delimiter + 1);
  if (mode == "context") {
    auto ctx = weave::Context::create();
    check(ctx);
    auto task = scenario_task(options, scenario);
    auto status = ctx->run(std::move(task));
    if (!status)
      std::fprintf(
        stderr,
        "Root error: code=%d category=%s message=%s\n",
        status.error().value(),
        status.error().category().name(),
        status.error().message().c_str());
    check(status);
  } else if (mode == "blocking") {
    if (scenario == "normal")
      blocking(options);
    else if (scenario == "early")
      blocking_early(options);
    else
      blocking_terminal(options, scenario);
  } else {
#if defined(WEAVE_POSTGRES_TEST_RUNTIME)
    auto scheduler = mode.ends_with("stealing") ? weave::Scheduler::work_stealing : weave::Scheduler::worker_affine;
    auto io = mode.starts_with("shared") ? weave::IoLayout::shared : weave::IoLayout::sharded;
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = io});
    check(runtime);
    std::vector<weave::JoinHandle<void>> jobs;
    for (unsigned index = 0; index < 16; ++index) {
      auto task = scenario_task(options, scenario);
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
  std::printf("Exchange chunks controls passed: %u checks\n", checks.load());
}
#endif
