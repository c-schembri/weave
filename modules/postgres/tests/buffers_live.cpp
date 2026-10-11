#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include <weave/timer.hpp>
#include "tls_certificates.hpp"
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include "runtime_fixture.hpp"
#endif
#include <iostream>
#include <source_location>

namespace pg = weave::pg;

struct PrimitiveRecord {
  weave::i64 id;
  weave::i16 rating;
  int quantity;
  float voltage;
  double temperature;
  bool active;
  std::string name;
  std::optional<std::string> notes;
};

static void check(bool value, std::source_location location = std::source_location::current())
{
  if (!value) {
    std::fprintf(stderr, "Buffer check failed: %s:%u\n", location.file_name(), location.line());
    std::exit(EXIT_FAILURE);
  }
}

static weave::Task<void> primitives(pg::Connection &database)
{
  const std::array formats{pg::Format::text, pg::Format::binary};
  for (auto format : formats) {
    auto result = co_await database.execute_borrowed(
      "SELECT 42::bigint AS id, (-32768)::smallint AS rating, 3::integer AS quantity, "
      "1.25::real AS voltage, (-4.5)::double precision AS temperature, "
      "true AS active, 'Ada'::text AS name, NULL::text AS notes",
      {},
      format);
    check(result.rows.size() == 1);
    const auto &row = result.rows.front();
    auto record = row.as<PrimitiveRecord>();
    check(record && record->id == 42 && record->rating == -32768 && record->quantity == 3);
    check(record->voltage == 1.25F && record->temperature == -4.5 && record->active && record->name == "Ada");
    check(!record->notes);
    auto decoder = result.decoder<PrimitiveRecord>();
    check(decoder.has_value());
    auto cached = decoder->decode(row);
    check(cached && cached->id == 42 && cached->name == "Ada" && !cached->notes);
    check(row["id"].as<weave::i64>() == 42);
    check(row.get<weave::i16>("rating") == -32768);
    check(row["quantity"].integer<int>() == 3);
    check(row["voltage"].floating() == 1.25);
    check(row.get<float>("temperature") == -4.5F);
    check(row["active"].boolean() == true);
    check(row["name"].string() == "Ada");
    auto notes = row["notes"].optional<std::string_view>();
    check(notes && !*notes);

    auto fields = row.reader();
    check(fields.next<weave::i64>() == 42);
    check(fields.next().integer<weave::i16>() == -32768);
    check(fields.skip(1).has_value());
    check(fields.next().as<double>() == 1.25);
    auto column = result.column("temperature");
    check(column.has_value());
    check(row[*column].floating() == -4.5);
    auto copied = result.copy();
    auto foreign = copied.rows.front()[*column].as<double>();
    check(!foreign && foreign.error() == pg::DecodeError::schema_mismatch);
    auto retained = row;
    result = {};
    check(record->name == "Ada");
    check(decoder->decode(retained).has_value());
    check(retained["name"].text() == "Ada");
    check(retained[*column].floating() == -4.5);
  }

  auto simple = co_await database.query("SELECT 7 AS id");
  check(simple.size() == 1 && simple.front().rows.front().get<int>("id") == 7);
  auto duplicate = co_await database.execute_borrowed("SELECT 1 AS id, 2 AS id");
  auto ambiguous = duplicate.rows.front()["id"].as<int>();
  check(!ambiguous && ambiguous.error() == pg::DecodeError::ambiguous_column);

  co_await database.start_rows("SELECT 9 AS streamed");
  auto retained = co_await database.read_row();
  check(retained && retained->get<int>("streamed") == 9);
  check(!(co_await database.read_row()));
  co_await database.start_rows("SELECT 10 AS next_stream");
  check(retained->get<int>("streamed") == 9);
  auto next = co_await database.read_row();
  check(next && next->get<int>("next_stream") == 10);
  check(!(co_await database.read_row()));

  auto exchange = co_await database.exchange("SELECT g AS value FROM generate_series(1, 3) g", {.chunk_rows = 1});
  int rows = 0;
  while (auto event = co_await exchange.next()) {
    if (auto *result = std::get_if<pg::ResultSet>(&*event)) {
      for (const auto &row : result->rows)
        check(row.get<int>("value") == ++rows);
    }
  }
  check(rows == 3 && exchange.finish().has_value());
}

static weave::Task<void> exercise(pg::Options options)
{
  auto database = co_await pg::connect(options);
  co_await primitives(database);
  std::string text(4096, 'x');
  const std::array bytes{char{0}, char{1}, char{2}, char{0}};
  std::array parameters{
    pg::ParameterView{.data = text, .type = 25},
    pg::ParameterView{.data = std::string_view{bytes.data(), bytes.size()}, .type = 17, .format = pg::Format::binary},
    pg::ParameterView{.type = 25}};
  const std::string_view sql = "SELECT octet_length($1::text), $2::bytea, $3::text";
  const std::array<weave::u32, 3> types{25, 17, 25};
  auto deferred = database.execute_borrowed(sql, parameters, pg::Format::binary);
  auto reset = co_await weave::as_result(database.reset(options));
  check(!reset && reset.error() == pg::Error::busy);
  auto result = co_await std::move(deferred);
  check(result.rows[0][0].binary_integer<int>() == 4096);
  check(result.rows[0][1].bytes() == std::string_view{bytes.data(), bytes.size()});
  check(result.rows[0][2].is_null());
  co_await database.prepare_borrowed("borrowed", sql, types);
  result = co_await database.execute_prepared_borrowed("borrowed", parameters, pg::Format::binary);
  check(result.rows[0][0].binary_integer<int>() == 4096);
  auto outcome = co_await database.execute_prepared_outcome_borrowed("borrowed", parameters);
  check(outcome.result && outcome.result->rows[0][0].integer<int>() == 4096);
  outcome = co_await database.execute_outcome_borrowed("SELECT 1/0");
  check(outcome.error.sqlstate() == "22012" && database.open());
  result = co_await database.execute_borrowed("SELECT 42");
  check(result.rows[0][0].integer<int>() == 42);
  auto invalid = co_await weave::as_result(database.execute_borrowed(std::string_view{"SELECT\0", 7}));
  check(!invalid && invalid.error() == std::errc::invalid_argument && database.open());

  co_await database.start_copy("COPY (SELECT repeat('x', 20) FROM generate_series(1, 3)) TO STDOUT");
  std::array<std::byte, 3> buffer;
  auto empty = co_await weave::as_result(database.read_copy(std::span<std::byte>{}));
  check(!empty && empty.error() == std::errc::invalid_argument && database.open());
  auto chunk = co_await database.read_copy(buffer);
  check(chunk && chunk->size == 3 && !chunk->end_of_message);
  auto replacement = database.on_trace({});
  check(!replacement);
  check(replacement.error() == pg::Error::busy);
  auto owning = co_await weave::as_result(database.read_copy());
  check(!owning && owning.error() == pg::Error::busy && database.open());
  std::string message(reinterpret_cast<const char *>(buffer.data()), chunk->size);
  std::size_t messages = 0;
  while (auto read = co_await database.read_copy(buffer)) {
    message.append(reinterpret_cast<const char *>(buffer.data()), read->size);
    if (read->end_of_message) {
      check(message == std::string(20, 'x') + "\n");
      message.clear();
      ++messages;
    }
  }
  check(messages == 3 && database.copy_result()->command == "COPY 3");

  std::vector<std::string> traces;
  auto observer = database.on_trace(
    {.handler =
        [&](const pg::TraceMessage &message) noexcept {
          if (message.direction == pg::TraceDirection::backend && message.kind == 'd') {
            check(message.length == 25 && !message.redacted && message.truncated);
            traces.emplace_back(reinterpret_cast<const char *>(message.payload.data()), message.payload.size());
          }
        },
      .content = pg::TraceContent::application,
      .payload_bytes = 5});
  check(observer.has_value());
  co_await database.start_copy("COPY (SELECT repeat('y', 20) FROM generate_series(1, 2)) TO STDOUT");
  while (co_await database.read_copy(buffer)) {
  }
  check(traces.size() == 2 && traces[0] == "yyyyy" && traces[1] == "yyyyy");
  check(database.on_trace({}).has_value());

  co_await database.start_copy("COPY (SELECT 'a' FROM generate_series(1, 2)) TO STDOUT");
  auto stale = database.read_copy(buffer);
  while (co_await database.read_copy()) {
  }
  co_await database.start_copy("COPY (SELECT 'b') TO STDOUT");
  auto rejected = co_await weave::as_result(std::move(stale));
  check(!rejected && rejected.error() == std::errc::invalid_argument && database.open());
  while (co_await database.read_copy(buffer)) {
  }
  co_await database.finish();
}

int main()
{
  fixture::Certificates files;
  std::printf("%s\n%s\n%s\n", files.ca.c_str(), files.leaf.c_str(), files.private_key.c_str());
  std::fflush(stdout);
  std::string port_text, password, standby, address, local;
  check(static_cast<bool>(std::getline(std::cin, port_text)));
  check(static_cast<bool>(std::getline(std::cin, password)));
  check(static_cast<bool>(std::getline(std::cin, standby)));
  check(static_cast<bool>(std::getline(std::cin, address)));
  check(static_cast<bool>(std::getline(std::cin, local)));
  auto port = weave::parse_port(port_text);
  check(port.has_value());
  pg::Options plain{
    .host = "127.0.0.1",
    .port = *port,
    .user = "weave",
    .database = "postgres",
    .password = password,
    .plaintext = true};
  plain.max_protocol = pg::ProtocolVersion::v30;
  auto secured = plain;
  auto credentials = weave::TlsContext::client(
    {.ca_file = files.ca, .certificate_file = files.client, .private_key_file = files.client_key});
  check(credentials.has_value());
  secured.tls = *credentials;
  secured.plaintext = false;
  secured.channel_binding = pg::ChannelBinding::require;
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  const std::array profiles{plain, secured};
  for (const auto &options : profiles) {
    auto result = ctx->run(weave::timeout(std::chrono::seconds{20}, exercise(options)));
    if (!result)
      return weave::report_error(result.error());
  }
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  for (auto layout : support::io_layouts) {
    auto runtime = weave::Runtime::create({.workers = 4, .io_layout = layout});
    check(runtime.has_value());
    auto result = runtime->run(exercise(secured));
    if (!result)
      return weave::report_error(result.error());
  }
#endif

  auto blocking = pg::BlockingConnection::connect(secured);
  check(blocking.has_value());
  const std::array parameters{pg::ParameterView{.data = "42", .type = 23}};
  const std::array<weave::u32, 1> types{23};
  auto rows = blocking->execute_borrowed("SELECT $1::int", parameters);
  check(rows && rows->rows[0][0].integer<int>() == 42);
  check(blocking->prepare_borrowed("blocking", "SELECT $1::int", types).has_value());
  rows = blocking->execute_prepared_borrowed("blocking", parameters);
  check(rows && rows->rows[0][0].integer<int>() == 42);
  auto outcome = blocking->execute_prepared_outcome_borrowed("blocking", parameters);
  check(outcome && outcome->result && outcome->result->rows[0][0].integer<int>() == 42);
  outcome = blocking->execute_outcome_borrowed("SELECT 1/0");
  check(outcome && outcome->error.sqlstate() == "22012");
  check(blocking->start_copy("COPY (SELECT repeat('z', 20)) TO STDOUT").has_value());
  std::array<std::byte, 3> buffer;
  std::string text;
  for (;;) {
    auto received = blocking->read_copy(buffer);
    check(received.has_value());
    if (!*received)
      break;
    text.append(reinterpret_cast<const char *>(buffer.data()), (*received)->size);
  }
  check(text == std::string(20, 'z') + "\n");
  check(blocking->finish().has_value());
}
