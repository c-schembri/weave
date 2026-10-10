#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/log.hpp>
#include <weave/timer.hpp>
#include <weave/port.hpp>
#include "tls_certificates.hpp"
#include "pipeline_live.hpp"
#include "replication_live.hpp"
#include "authentication_live.hpp"
#include <cstdio>
#include <iostream>
#include <thread>
#include <fstream>
#include <memory>
#if !defined(_WIN32)
#include <unistd.h>
#endif
#ifdef WEAVE_TEST_RUNTIME
#include <weave/runtime.hpp>
#endif

static weave::Task<void> cancel_query(weave::pg::Connection &connection)
{
  for (unsigned attempt = 0; attempt < 200; ++attempt) {
    auto notices = connection.take_notices();
    for (const auto &notice : notices) {
      if (notice.message() == "weave_cancel_ready") {
        co_await connection.request_cancel();
        co_return;
      }
    }
    co_await weave::sleep_for(std::chrono::milliseconds{10});
  }
  co_await weave::fail(std::errc::timed_out);
}

static weave::Task<void> slow_query(weave::pg::Connection &connection)
{
  auto result = co_await weave::as_result(
    connection.query("DO $$BEGIN RAISE NOTICE 'weave_cancel_ready'; PERFORM pg_sleep(30); END$$"));
  if (result || weave::pg::sqlstate(result.error()) != "57014")
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> functions_and_encoding(weave::pg::Connection &connection)
{
  auto oid = co_await connection.query("SELECT 'pg_catalog.int4pl(integer,integer)'::regprocedure::oid");
  auto function = oid.front().rows.front().front().integer<weave::u32>();
  if (!function)
    co_await weave::fail(function.error());

  std::vector<weave::pg::Parameter> parameters{{"40"}, {"2"}};
  auto answer = co_await connection.call_function(*function, parameters);
  if (answer.binary_integer<weave::i32>() != 42)
    co_await weave::fail(std::errc::bad_message);

  auto text_answer = co_await connection.call_function(*function, parameters, weave::pg::Format::text);
  if (text_answer.integer<int>() != 42)
    co_await weave::fail(std::errc::bad_message);
  parameters.front().data.reset();
  if (!(co_await connection.call_function(*function, parameters)).is_null())
    co_await weave::fail(std::errc::bad_message);

  auto invalid = co_await weave::as_result(connection.call_function(0xffffffff));
  if (invalid || weave::pg::sqlstate(invalid.error()) != "42883" || !connection.open())
    co_await weave::fail(std::errc::bad_message);

  std::string text = "'\\; SELECT 1; --";
  auto literal = connection.escape_literal(text);
  auto identifier = connection.escape_identifier("a\" b");
  if (!literal || !identifier)
    co_await weave::fail(std::errc::bad_message);

  const std::array settings{"on", "off"};
  for (auto setting : settings) {
    co_await connection.query(std::string{"SET standard_conforming_strings="} + setting);
    auto quoted = co_await connection.query("SELECT " + *literal + " AS " + *identifier);
    if (quoted.front().rows.front().front().bytes() != text || quoted.front().columns.front().name != "a\" b")
      co_await weave::fail(std::errc::bad_message);
  }

  co_await connection.query("SET standard_conforming_strings=on; SET client_encoding=LATIN1");
  if (connection.client_encoding() != weave::pg::Encoding::latin1 || !connection.escape_literal("\xe9") ||
    !connection.escape_identifier("hello"))
    co_await weave::fail(std::errc::bad_message);
  co_await connection.set_client_encoding(weave::pg::Encoding::utf8);

  const std::array bytes{std::byte{0}, std::byte{255}, std::byte{'\\'}, std::byte{'\''}};
  auto encoded = weave::pg::encode_bytea(bytes);
  if (!encoded)
    co_await weave::fail(encoded.error());
  auto escaped = connection.escape_literal(*encoded);
  if (!escaped)
    co_await weave::fail(escaped.error());

  auto result = co_await connection.query("SELECT " + *escaped + "::bytea");
  if (weave::pg::decode_bytea(result.front().rows.front().front().bytes()) !=
    std::vector<std::byte>(bytes.begin(), bytes.end()))
    co_await weave::fail(std::errc::bad_message);
}

static weave::Task<void> startup_diagnostic(weave::pg::Options options)
{
  options.password = "intentionally-wrong-test-password";
  weave::pg::Diagnostic diagnostic;
  auto result = co_await weave::as_result(weave::pg::connect(std::move(options), diagnostic));
  if (result || weave::pg::sqlstate(result.error()) != "28P01" || diagnostic.sqlstate() != "28P01" ||
    diagnostic.message().empty())
    co_await weave::fail(std::errc::bad_message);
}

static std::string keyword_value(std::string_view text)
{
  std::string quoted{"'"};
  for (auto byte : text) {
    if (byte == '\'' || byte == '\\')
      quoted.push_back('\\');
    quoted.push_back(byte);
  }
  quoted.push_back('\'');
  return quoted;
}

static weave::Task<void> parsed_options(const weave::pg::Options &profile, const fixture::Certificates &certificates)
{
  std::string text = "host=localhost hostaddr=127.0.0.1 port=" + std::to_string(profile.port) +
    " user=weave dbname=postgres password=" + keyword_value(profile.password) +
    " application_name='weave parsed' client_encoding=LATIN1 target_session_attrs=primary";
  if (profile.plaintext)
    text += " sslmode=disable";
  else {
    text += " sslmode=verify-full channel_binding=require sslrootcert=" + keyword_value(certificates.ca) +
      " sslcert=" + keyword_value(certificates.client) + " sslkey=" + keyword_value(certificates.client_key);
  }

  auto options = weave::pg::Options::parse(text);
  if (!options)
    co_await weave::fail(options.error());
  auto connection = co_await weave::pg::connect(*options);
  if (connection.client_encoding() != weave::pg::Encoding::latin1 || !connection.escape_literal("text"))
    co_await weave::fail(std::errc::bad_message);
  auto result = co_await connection.query("SELECT current_setting('application_name'), 42");
  if (result.front().rows.front().front().bytes() != "weave parsed" ||
    result.front().rows.front()[1].integer<int>() != 42)
    co_await weave::fail(std::errc::bad_message);

  auto process = connection.backend_process();
  co_await connection.reset(*options);
  auto encoding = connection.parameter("client_encoding");
  if (connection.backend_process() == process || !encoding || *encoding != "LATIN1")
    co_await weave::fail(std::errc::bad_message);
  co_await connection.finish();
}

static weave::Task<void> loaded_options(const weave::pg::Options &profile, const fixture::Certificates &certificates)
{
  auto reservation = co_await weave::tcp::listen("127.0.0.1", 0);
  auto unavailable = reservation.local_port();
  if (auto closed = reservation.close(); !closed)
    co_await weave::fail(closed.error());

  auto file = certificates.directory / "connection-password";
  std::ofstream output(file, std::ios::binary);
  output << "localhost:" << unavailable << ":postgres:weave:unused\n";
  output << "localhost:" << profile.port << ":postgres:weave:" << profile.password << '\n';
  output.close();
  if (!output)
    co_await weave::fail(std::errc::io_error);
  std::error_code file_error;
  std::filesystem::permissions(
    file,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace,
    file_error);
  if (file_error)
    co_await weave::fail(file_error);

  std::string text = "postgres://weave@localhost:" + std::to_string(unavailable) +
    ",localhost:" + std::to_string(profile.port) + "/postgres?hostaddr=127.0.0.1,127.0.0.1&sslmode=";
  text += profile.plaintext ? "disable" : "verify-full";
  auto bytes = file.u8string();
  auto options = weave::pg::Options::load(
    text,
    {.environment = false, .user_files = false, .password_file = std::string{bytes.begin(), bytes.end()}});
  std::filesystem::remove(file, file_error);
  if (file_error)
    co_await weave::fail(file_error);
  if (!options)
    co_await weave::fail(options.error());
  if (options->hosts.size() != 2 || options->hosts[0].password != "unused" ||
    options->hosts[1].password != profile.password)
    co_await weave::fail(std::errc::bad_message);
  options->tls = profile.tls;
  options->channel_binding = profile.channel_binding;
  options->settings = {{"TimeZone", "UTC"}, {"geqo", "off"}};

  auto connection = co_await weave::pg::connect(*options);
  auto result = co_await connection.query("SELECT current_setting('TimeZone'), current_setting('geqo'), 42");
  if (result.front().rows.front()[0].bytes() != "UTC" || result.front().rows.front()[1].bytes() != "off" ||
    result.front().rows.front()[2].integer<int>() != 42)
    co_await weave::fail(std::errc::bad_message);
  auto process = connection.backend_process();
  co_await connection.reset(*options);
  if (connection.backend_process() == process)
    co_await weave::fail(std::errc::bad_message);
  co_await connection.finish();
}

static weave::Task<void> large_objects(weave::pg::Connection &connection)
{
  namespace lo = weave::pg::lo;
  auto outside = co_await weave::as_result(lo::create(connection));
  if (outside || outside.error() != std::errc::operation_not_permitted || !connection.open())
    co_await weave::fail(std::errc::bad_message);

  co_await connection.query("BEGIN");
  co_await connection.query(
    "CREATE TEMP TABLE lo_schema_marker(value int);"
    "CREATE TYPE pg_temp.oid AS ENUM ('shadow');"
    "SET LOCAL search_path=pg_temp,public,pg_catalog");
  auto object = co_await lo::create(connection);
  auto descriptor = co_await lo::open(connection, object, lo::Access::read_write);
  std::vector<std::byte> bytes(65537);
  for (std::size_t index = 0; index < bytes.size(); ++index)
    bytes[index] = static_cast<std::byte>(index & 255);

  co_await lo::write_all(connection, descriptor, bytes);
  if (co_await lo::tell(connection, descriptor) != static_cast<weave::i64>(bytes.size()))
    co_await weave::fail(std::errc::bad_message);
  if (co_await lo::seek(connection, descriptor, 0) != 0)
    co_await weave::fail(std::errc::bad_message);

  auto first = co_await lo::read(connection, descriptor, 100);
  if (first != std::vector<std::byte>(bytes.begin(), bytes.begin() + 100))
    co_await weave::fail(std::errc::bad_message);
  co_await lo::truncate(connection, descriptor, 3);
  if (co_await lo::tell(connection, descriptor) != 100)
    co_await weave::fail(std::errc::bad_message);

  co_await lo::seek(connection, descriptor, 0);
  if (co_await lo::read(connection, descriptor, 100) != std::vector<std::byte>(bytes.begin(), bytes.begin() + 3))
    co_await weave::fail(std::errc::bad_message);

  constexpr weave::i64 distant = weave::i64{1} << 33;
  co_await lo::seek(connection, descriptor, distant);
  const std::array last{std::byte{42}};
  if (co_await lo::write(connection, descriptor, last) != 1 || co_await lo::tell(connection, descriptor) != distant + 1)
    co_await weave::fail(std::errc::bad_message);

  co_await lo::seek(connection, descriptor, distant - 1);
  const std::vector expected{std::byte{0}, std::byte{42}};
  if (co_await lo::read(connection, descriptor, 2) != expected)
    co_await weave::fail(std::errc::bad_message);

  co_await lo::truncate(connection, descriptor, 0);
  co_await lo::close(connection, descriptor);
  co_await lo::remove(connection, object);
  if (co_await lo::create(connection, object) != object)
    co_await weave::fail(std::errc::bad_message);
  co_await lo::remove(connection, object);

  auto negative = co_await weave::as_result(lo::read(connection, -1));
  auto oversized = co_await weave::as_result(lo::read(connection, descriptor, 1024ULL * 1024 * 1024));
  if (negative || negative.error() != std::errc::invalid_argument || oversized ||
    oversized.error() != weave::pg::Error::resource_limit || connection.transaction() != weave::pg::Transaction::active)
    co_await weave::fail(std::errc::bad_message);

  co_await connection.query("ROLLBACK");
}

struct LiveNotices {
  std::vector<weave::pg::Diagnostic> values;
};

static weave::Task<void> notice_policies(weave::pg::Connection &connection, weave::pg::Options options)
{
  auto observed = std::make_shared<LiveNotices>();
  auto previous = connection.on_notice([observed](const weave::pg::Diagnostic &notice) noexcept {
    observed->values.push_back(notice);
  });
  if (!previous)
    co_await weave::fail(previous.error());

  constexpr auto notices = "DO $$BEGIN RAISE NOTICE 'weave_notice'; RAISE WARNING 'weave_warning'; END$$";
  co_await connection.query(notices);
  if (observed->values.size() != 2 || observed->values[0].message() != "weave_notice" ||
    observed->values[1].message() != "weave_warning" || observed->values[1].sqlstate() != "01000")
    co_await weave::fail(std::errc::bad_message);

  co_await connection.reset(std::move(options));
  co_await connection.query(notices);
  if (observed->values.size() != 4 || !connection.take_notices().empty())
    co_await weave::fail(std::errc::bad_message);

  auto restored = connection.on_notice(std::move(*previous));
  if (!restored)
    co_await weave::fail(restored.error());
  co_await connection.query(notices);
  auto queued = connection.take_notices();
  if (queued.size() != 2 || queued.front().message() != "weave_notice" || observed->values.size() != 4)
    co_await weave::fail(std::errc::bad_message);
}

struct QualificationProgress {
  struct Stage {
    const char *name;
    std::chrono::steady_clock::time_point started;
  };

  std::size_t client;
  std::array<Stage, 16> stages{};
  std::size_t count = 0;
  bool complete = false;

  void enter(const char *name)
  {
    stages[count++] = {name, std::chrono::steady_clock::now()};
  }

  ~QualificationProgress()
  {
    if (complete)
      return;

    auto finished = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < count; ++index) {
      auto end = index + 1 < count ? stages[index + 1].started : finished;
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - stages[index].started).count();
      std::fprintf(
        stderr,
        "Qualification client %zu: %s %lld ms%s\n",
        client,
        stages[index].name,
        static_cast<long long>(elapsed),
        index + 1 == count ? " (unfinished)" : "");
    }
  }
};

static weave::Task<void> probe(weave::pg::Options options, std::size_t client = 0)
{
  QualificationProgress progress{client};
  progress.enter("startup");
  auto connection = co_await weave::pg::connect(options);

  progress.enter("notices and reset");
  co_await notice_policies(connection, options);

  progress.enter("pipeline");
  co_await fixture::pipeline(connection);

  progress.enter("functions and encoding");
  co_await functions_and_encoding(connection);

  progress.enter("large objects");
  co_await large_objects(connection);

  progress.enter("queries and prepare");
  auto simple = co_await connection.query("SELECT 42, NULL, ''::text; SELECT 'second'");
  if (simple.size() != 2 || simple[0].rows[0][0].bytes() != "42" || !simple[0].rows[0][1].is_null() ||
    simple[0].rows[0][2].bytes() != "")
    co_await weave::fail(std::errc::bad_message);

  std::vector<weave::pg::Parameter> parameters{{"73", 23}, {std::nullopt, 25}};
  auto extended = co_await connection.execute("SELECT $1::int, $2::text", parameters);
  if (extended.rows[0][0].bytes() != "73" || !extended.rows[0][1].is_null())
    co_await weave::fail(std::errc::bad_message);

  auto binary = co_await connection.execute("SELECT $1::int, $2::text", parameters, weave::pg::Format::binary);
  if (binary.rows[0][0].binary_integer<weave::i32>() != 73 || !binary.rows[0][1].is_null())
    co_await weave::fail(std::errc::bad_message);

  std::vector<weave::u32> types{23};
  co_await connection.prepare("answer", "SELECT $1 + 1", types);
  auto described = co_await connection.describe("answer");
  if (described.parameter_types != types || described.columns.size() != 1)
    co_await weave::fail(std::errc::bad_message);

  parameters.resize(1);
  auto prepared = co_await connection.execute_prepared("answer", parameters);
  if (prepared.rows[0][0].bytes() != "74")
    co_await weave::fail(std::errc::bad_message);

  auto invalid = co_await weave::as_result(connection.query("SELECT missing_column"));
  if (invalid || weave::pg::sqlstate(invalid.error()) != "42703" || !connection.open())
    co_await weave::fail(std::errc::bad_message);

  auto recovered = co_await connection.query("SELECT 1");
  if (recovered[0].rows[0][0].bytes() != "1")
    co_await weave::fail(std::errc::bad_message);

  co_await connection.close_prepared("answer");
  progress.enter("batch");
  std::vector<weave::pg::Command> batch{{"SELECT 1"}, {"SELECT missing_column"}, {"SELECT 3"}};
  auto outcomes = co_await connection.batch(batch);
  if (outcomes.size() != 3 || !outcomes[0].result || outcomes[1].error.sqlstate() != "42703" || !outcomes[2].aborted)
    co_await weave::fail(std::errc::bad_message);

  batch.assign(32, {"SELECT repeat('x', 65536)"});
  auto duplex = co_await connection.batch(std::move(batch));
  for (const auto &outcome : duplex) {
    if (!outcome.result || outcome.result->rows[0][0].bytes().size() != 65536)
      co_await weave::fail(std::errc::bad_message);
  }

  progress.enter("COPY");
  co_await connection.query("CREATE TEMP TABLE sample (value int, text text)");
  co_await connection.start_copy("COPY sample FROM STDIN");
  std::string data = "1\tfirst\n2\tsecond\n";
  co_await connection.write_copy(std::as_bytes(std::span{data.data(), data.size()}));
  auto copied = co_await connection.end_copy();
  if (copied.command != "COPY 2")
    co_await weave::fail(std::errc::bad_message);

  co_await connection.start_copy("COPY sample TO STDOUT");
  std::string output;
  while (auto chunk = co_await connection.read_copy())
    output.append(reinterpret_cast<const char *>(chunk->data()), chunk->size());
  if (output != data || connection.copy_result()->command != "COPY 2")
    co_await weave::fail(std::errc::bad_message);

  co_await connection.start_copy("COPY sample FROM STDIN");
  auto failed_copy = co_await weave::as_result(connection.end_copy("client aborted"));
  if (failed_copy || weave::pg::sqlstate(failed_copy.error()) != "57014" || !connection.open())
    co_await weave::fail(std::errc::bad_message);

  progress.enter("cancellation");
  co_await weave::timeout(std::chrono::seconds{5}, weave::when_all(slow_query(connection), cancel_query(connection)));
  co_await connection.query("SELECT 1");
  progress.enter("rows");
  co_await connection.start_rows("SELECT i FROM generate_series(1, 1024) i");
  int expected = 1;
  while (auto row = co_await connection.read_row()) {
    auto value = row->front().integer<int>();
    if (!value || *value != expected++ || connection.row_columns().front().type != 23)
      co_await weave::fail(std::errc::bad_message);
  }
  if (expected != 1025)
    co_await weave::fail(std::errc::bad_message);

  progress.enter("portal");
  co_await connection.query("BEGIN");
  co_await connection.open_portal("chunks", "SELECT i FROM generate_series(1, 257) i");
  expected = 1;
  for (;;) {
    auto chunk = co_await connection.fetch("chunks", 64);
    for (const auto &row : chunk.rows) {
      auto value = row.front().integer<int>();
      if (!value || *value != expected++)
        co_await weave::fail(std::errc::bad_message);
    }
    if (!chunk.suspended)
      break;
  }
  if (expected != 258)
    co_await weave::fail(std::errc::bad_message);
  co_await connection.close_portal("chunks");
  co_await connection.query("COMMIT");

  progress.enter("finish");
  co_await connection.finish();
  progress.complete = true;
}

static weave::Result<void> large_object_files(
  weave::pg::BlockingConnection &connection,
  const std::filesystem::path &directory)
{
  namespace lo = weave::pg::lo;
  auto input = directory / std::filesystem::path{u8"large-object-\u00e9-input.bin"};
  auto output = directory / std::filesystem::path{u8"large-object-\u00e9-output.bin"};

  struct Cleanup {
    std::filesystem::path input, output;

    ~Cleanup()
    {
      std::error_code error;
      std::filesystem::remove(input, error);
      std::filesystem::remove(output, error);
    }
  } cleanup{input, output};

  std::string data(100000, '\0');
  for (std::size_t index = 0; index < data.size(); ++index)
    data[index] = static_cast<char>(index & 255);
  {
    std::ofstream file(input, std::ios::binary);
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    if (!file)
      return std::unexpected(std::make_error_code(std::errc::io_error));
  }

  if (auto transaction = connection.query("BEGIN"); !transaction)
    return std::unexpected(transaction.error());
  auto object = lo::import_file(connection, input);
  if (!object)
    return std::unexpected(object.error());
  if (auto exported = lo::export_file(connection, *object, output); !exported)
    return std::unexpected(exported.error());

  std::ifstream file(output, std::ios::binary);
  std::string received((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>{});
  file.close();
  if (received != data)
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto missing = lo::import_file(connection, directory / "missing-input");
  auto bad_target = lo::export_file(connection, *object, directory / "missing-parent" / "output");
  if (missing || missing.error() != std::errc::no_such_file_or_directory || bad_target ||
    !lo::remove(connection, *object))
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto not_file = lo::import_file(connection, directory);
  if (not_file || connection.transaction() != weave::pg::Transaction::active)
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto rolled_back = connection.query("ROLLBACK");
  if (!rolled_back)
    return std::unexpected(rolled_back.error());

  return {};
}

static weave::Task<void> connection_options(
  weave::Context &ctx,
  weave::pg::Options options,
  weave::u16 standby_port,
  weave::IpAddress address)
{
  namespace pg = weave::pg;
  auto unavailable = weave::tcp::listen(ctx, "127.0.0.1", 0);
  if (!unavailable)
    co_await weave::fail(unavailable.error());
  auto unavailable_port = unavailable->local_port();
  if (auto closed = unavailable->close(); !closed)
    co_await weave::fail(closed.error());
  options.hosts = {{"127.0.0.1", unavailable_port, {}}, {"localhost", options.port, address}};
  auto connection = co_await pg::connect(options);
  {
    auto pending = connection.query("SELECT 42");
    auto rejected = co_await weave::as_result(connection.reset(options));
    if (rejected || rejected.error() != pg::make_error_code(pg::Error::busy) || !connection.open())
      co_await weave::fail(std::errc::bad_message);
  }
  co_await connection.reset(options);
  auto process = connection.backend_process();
  co_await connection.prepare("old", "SELECT 42");
  co_await connection.query("BEGIN; CREATE TEMP TABLE old_session(value int)");
  co_await connection.reset(options);
  if (!connection.open() || connection.backend_process() == process ||
    connection.transaction() != pg::Transaction::idle)
    co_await weave::fail(std::errc::bad_message);
  auto missing = co_await weave::as_result(connection.execute_prepared("old"));
  if (missing || pg::sqlstate(missing.error()) != "26000" || !connection.open())
    co_await weave::fail(std::errc::bad_message);
  auto old_table = co_await weave::as_result(connection.query("SELECT * FROM old_session"));
  if (old_table || pg::sqlstate(old_table.error()) != "42P01")
    co_await weave::fail(std::errc::bad_message);

  auto invalid = options;
  invalid.hosts.front().port = 0;
  auto rejected = co_await weave::as_result(connection.reset(invalid));
  if (rejected || rejected.error() != std::errc::invalid_argument || !connection.open())
    co_await weave::fail(std::errc::bad_message);

  auto query = ctx.spawn(connection.query("SELECT pg_catalog.pg_sleep(0.1)"));
  if (!query)
    co_await weave::fail(query.error());
  co_await weave::sleep_for(std::chrono::milliseconds{20});
  auto busy = co_await weave::as_result(connection.reset(options));
  if (busy || busy.error() != pg::make_error_code(pg::Error::busy))
    co_await weave::fail(std::errc::bad_message);
  co_await std::move(*query);

  auto bad_password = options;
  bad_password.password = "intentionally_incorrect_password";
  pg::Diagnostic diagnostic;
  auto failed = co_await weave::as_result(connection.reset(bad_password, diagnostic));
  if (failed || pg::sqlstate(failed.error()) != "28P01" || diagnostic.sqlstate() != "28P01" ||
    connection.last_error().sqlstate() != "28P01" || connection.open())
    co_await weave::fail(std::errc::bad_message);
  co_await connection.reset(options);
  co_await connection.finish();
  co_await connection.reset(options);
  co_await connection.finish();

  const std::array accepted{
    pg::TargetSession::read_write,
    pg::TargetSession::primary,
    pg::TargetSession::prefer_standby};
  for (auto target : accepted) {
    auto candidate = options;
    candidate.target_session = target;
    auto selected = co_await pg::connect(std::move(candidate));
    co_await selected.finish();
  }

  options.server_options = "-c default_transaction_read_only=on";
  options.target_session = pg::TargetSession::read_only;
  auto read_only = co_await pg::connect(options);
  co_await read_only.finish();
  options.target_session = pg::TargetSession::read_write;
  auto mismatch = co_await weave::as_result(pg::connect(options));
  if (mismatch || mismatch.error() != pg::make_error_code(pg::Error::target_session))
    co_await weave::fail(std::errc::bad_message);
  options.target_session = pg::TargetSession::standby;
  auto no_standby = co_await weave::as_result(pg::connect(options));
  if (no_standby || no_standby.error() != pg::make_error_code(pg::Error::target_session))
    co_await weave::fail(std::errc::bad_message);

  options.server_options.clear();
  if (!options.plaintext) {
    auto mismatched_name = options;
    mismatched_name.target_session = pg::TargetSession::any;
    mismatched_name.hosts = {{"invalid.example", options.port, address}, {"localhost", options.port, address}};
    auto invalid_certificate = co_await weave::as_result(pg::connect(std::move(mismatched_name)));
    if (invalid_certificate ||
      invalid_certificate.error() != weave::make_error_code(weave::TlsError::certificate_verification))
      co_await weave::fail(std::errc::bad_message);
  }
  options.hosts = {{"localhost", options.port, address}, {"localhost", standby_port, address}};
  const std::array standby_targets{
    pg::TargetSession::standby,
    pg::TargetSession::prefer_standby,
    pg::TargetSession::read_only};
  for (auto target : standby_targets) {
    options.target_session = target;
    auto selected = co_await pg::connect(options);
    auto recovery = co_await selected.query("SELECT pg_catalog.pg_is_in_recovery()");
    if (recovery.front().rows.front().front().bytes() != "t")
      co_await weave::fail(std::errc::bad_message);
    co_await selected.finish();
  }
  std::reverse(options.hosts.begin(), options.hosts.end());
  const std::array primary_targets{pg::TargetSession::primary, pg::TargetSession::read_write};
  for (auto target : primary_targets) {
    options.target_session = target;
    auto selected = co_await pg::connect(options);
    auto recovery = co_await selected.query("SELECT pg_catalog.pg_is_in_recovery()");
    if (recovery.front().rows.front().front().bytes() != "f")
      co_await weave::fail(std::errc::bad_message);
    co_await selected.finish();
  }
}

static weave::Task<void> balanced_connections(
  weave::pg::Options options,
  weave::u16 standby_port,
  weave::IpAddress address)
{
  namespace pg = weave::pg;
  options.host_balance = pg::HostBalance::random;
  options.hosts = {
    {"localhost", options.port, address, options.password},
    {"localhost", standby_port, address, options.password}};
  options.password = "wrong_global_password";

  for (unsigned repetition = 0; repetition < 8; ++repetition) {
    auto connection = co_await pg::connect(options);
    auto rows = co_await connection.query("SELECT pg_catalog.pg_is_in_recovery()");
    auto recovery = rows.front().rows.front().front().bytes();
    if (recovery != "t" && recovery != "f")
      co_await weave::fail(std::errc::bad_message);
    co_await connection.finish();
  }

  const std::array targets{
    pg::TargetSession::primary,
    pg::TargetSession::standby,
    pg::TargetSession::read_write,
    pg::TargetSession::read_only,
    pg::TargetSession::prefer_standby};
  for (auto target : targets) {
    options.target_session = target;
    auto connection = co_await pg::connect(options);
    auto rows = co_await connection.query("SELECT pg_catalog.pg_is_in_recovery()");
    bool expect_standby = target == pg::TargetSession::standby || target == pg::TargetSession::read_only ||
      target == pg::TargetSession::prefer_standby;
    if (rows.front().rows.front().front().bytes() != (expect_standby ? "t" : "f"))
      co_await weave::fail(std::errc::bad_message);
    co_await connection.reset(options);
    co_await connection.finish();
  }

  options.hosts.pop_back();
  options.target_session = pg::TargetSession::prefer_standby;
  auto fallback = co_await pg::connect(options);
  auto rows = co_await fallback.query("SELECT pg_catalog.pg_is_in_recovery()");
  if (rows.front().rows.front().front().bytes() != "f")
    co_await weave::fail(std::errc::bad_message);
  co_await fallback.finish();
}

static weave::Result<void> balanced_blocking(
  weave::pg::Options options,
  weave::u16 standby_port,
  weave::IpAddress address)
{
  namespace pg = weave::pg;
  options.host_balance = pg::HostBalance::random;
  options.hosts = {{"localhost", options.port, address}, {"localhost", standby_port, address}};
  options.target_session = pg::TargetSession::standby;
  auto connection = pg::BlockingConnection::connect(options);
  if (!connection)
    return std::unexpected(connection.error());
  auto rows = connection->query("SELECT pg_catalog.pg_is_in_recovery()");
  if (!rows)
    return std::unexpected(rows.error());
  if (rows->front().rows.front().front().bytes() != "t")
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  options.target_session = pg::TargetSession::primary;
  if (auto reset = connection->reset(options); !reset)
    return reset;
  rows = connection->query("SELECT pg_catalog.pg_is_in_recovery()");
  if (!rows)
    return std::unexpected(rows.error());
  if (rows->front().rows.front().front().bytes() != "f")
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  return connection->finish();
}

static weave::Result<void> blocking(weave::pg::Options options, const std::filesystem::path &directory)
{
  auto connection = weave::pg::BlockingConnection::connect(options);
  if (!connection)
    return std::unexpected(connection.error());
  if (auto result = fixture::pipeline(*connection); !result)
    return std::unexpected(result.error());
  if (auto files = large_object_files(*connection, directory); !files)
    return std::unexpected(files.error());

  auto rows = connection->query("SELECT 42");
  if (!rows || rows->front().rows.front().front().integer<int>() != 42)
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto encoding = connection->parameter("client_encoding");
  if (!connection->open() || connection->backend_process() == 0 || !encoding || *encoding != "UTF8" ||
    connection->transaction() != weave::pg::Transaction::idle || !connection->escape_literal("'\\") ||
    !connection->escape_identifier("a\"b"))
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  auto oid = connection->query("SELECT 'pg_catalog.int4pl(integer,integer)'::regprocedure::oid");
  if (!oid)
    return std::unexpected(oid.error());
  auto function = oid->front().rows.front().front().integer<weave::u32>();
  if (!function)
    return std::unexpected(function.error());

  std::vector<weave::pg::Parameter> parameters{{"40"}, {"2"}};
  auto answer = connection->call_function(*function, parameters);
  if (!answer || answer->binary_integer<weave::i32>() != 42)
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  namespace lo = weave::pg::lo;
  if (auto transaction = connection->query("BEGIN"); !transaction)
    return std::unexpected(transaction.error());
  auto object = lo::create(*connection);
  if (!object)
    return std::unexpected(object.error());
  auto descriptor = lo::open(*connection, *object, lo::Access::read_write);
  if (!descriptor)
    return std::unexpected(descriptor.error());

  const std::array bytes{std::byte{0}, std::byte{42}, std::byte{255}};
  if (!lo::write_all(*connection, *descriptor, bytes) || lo::tell(*connection, *descriptor) != 3 ||
    lo::seek(*connection, *descriptor, 0) != 0 ||
    lo::read(*connection, *descriptor) != std::vector<std::byte>(bytes.begin(), bytes.end()) ||
    !lo::truncate(*connection, *descriptor, 0) || !lo::close(*connection, *descriptor) ||
    !lo::remove(*connection, *object))
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  if (auto transaction = connection->query("ROLLBACK"); !transaction)
    return std::unexpected(transaction.error());

  auto cancellation = connection->cancel_handle();
  if (!cancellation)
    return std::unexpected(cancellation.error());

  std::error_code cancel_error;
  auto process = connection->backend_process();
  std::jthread cancel([handle = *cancellation, options, process, &cancel_error] {
    auto observer_options = options;
    observer_options.connect_timeout = std::chrono::seconds{2};
    auto observer = weave::pg::BlockingConnection::connect(std::move(observer_options));
    if (!observer) {
      cancel_error = observer.error();
      return;
    }

    auto active_query = "SELECT EXISTS (SELECT 1 FROM pg_catalog.pg_stat_activity WHERE pid = " +
      std::to_string(process) + " AND state = 'active' AND wait_event = 'PgSleep')";
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
      auto active = observer->query(active_query);
      if (!active) {
        cancel_error = active.error();
        return;
      }
      if (active->front().rows.front().front().bytes() == "t") {
        auto result = handle.request_blocking();
        if (!result)
          cancel_error = result.error();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    cancel_error = std::make_error_code(std::errc::timed_out);
  });
  auto interrupted = connection->query("SELECT pg_sleep(5)");
  cancel.join();
  if (cancel_error)
    return std::unexpected(cancel_error);
  if (interrupted || weave::pg::sqlstate(interrupted.error()) != "57014")
    return std::unexpected(std::make_error_code(std::errc::bad_message));

  if (auto reset = connection->reset(options); !reset)
    return std::unexpected(reset.error());
  if (!connection->open() || connection->backend_process() == process)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  return connection->finish();
}

static weave::pg::Options key_options(
  weave::pg::Options options,
  const weave::pg::ScramKey &client_key,
  const weave::pg::ScramKey &server_key,
  std::size_t index)
{
  auto kind = index % 8;
  if (kind < 4)
    options.scram_client_key = client_key;
  if (kind < 2 || (kind >= 4 && kind < 6))
    options.scram_server_key = server_key;
  if (kind < 2)
    options.password.clear();
  options.authentication.methods = {weave::pg::Authentication::scram_sha256};
  return options;
}

int main()
{
  fixture::Certificates certificates;
  std::printf("%s\n%s\n%s\n", certificates.ca.c_str(), certificates.leaf.c_str(), certificates.private_key.c_str());
  std::fflush(stdout);
  std::string port_text, password, standby_text, address_text, local_directory;
  std::getline(std::cin, port_text);
  std::getline(std::cin, password);
  std::getline(std::cin, standby_text);
  std::getline(std::cin, address_text);
  std::getline(std::cin, local_directory);
  std::string client_key_text, server_key_text;
  std::getline(std::cin, client_key_text);
  std::getline(std::cin, server_key_text);
  bool passthrough = !client_key_text.empty() || !server_key_text.empty();
  std::optional<weave::pg::ScramKey> client_key, server_key;
  if (passthrough) {
    auto parsed_client = weave::pg::ScramKey::parse(client_key_text);
    auto parsed_server = weave::pg::ScramKey::parse(server_key_text);
    if (!parsed_client || !parsed_server)
      return 2;
    client_key = std::move(*parsed_client);
    server_key = std::move(*parsed_server);
  }

  auto port = weave::parse_port(port_text);
  auto standby_port = weave::parse_port(standby_text);
  auto client_address = weave::IpAddress::parse(address_text);
  if (!port || !standby_port || !client_address)
    return 2;

  auto credentials = weave::TlsContext::client(
    {.ca_file = certificates.ca, .certificate_file = certificates.client, .private_key_file = certificates.client_key});
  if (!credentials)
    return weave::report_error(credentials.error());

  weave::pg::Options plain;
  plain.host = "127.0.0.1";
  plain.port = *port;
  plain.user = "weave";
  plain.database = "postgres";
  plain.password = password;
  plain.plaintext = true;
  plain.hosts = {{plain.host, *port, *client_address}};
  auto secured = plain;
  secured.plaintext = false;
  secured.tls = *credentials;
  secured.channel_binding = weave::pg::ChannelBinding::require;

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  std::fprintf(stderr, "Qualification authentication: SCRAM-PLUS, MD5/password opt-ins and protocol bounds\n");
  if (auto result = ctx->run(fixture::authentication(secured)); !result)
    return weave::report_error(result.error());
  if (auto result = fixture::authentication_blocking(secured); !result)
    return weave::report_error(result.error());

  const std::array profiles{plain, secured};
  for (const auto &profile : profiles) {
    const char *transport = profile.plaintext ? "plaintext" : "mTLS";
    std::fprintf(stderr, "Qualification %s: protocol and configuration\n", transport);
    if (auto result = ctx->run(parsed_options(profile, certificates)); !result)
      return weave::report_error(result.error());
    if (auto result = ctx->run(loaded_options(profile, certificates)); !result)
      return weave::report_error(result.error());
    if (auto result = ctx->run(startup_diagnostic(profile)); !result)
      return weave::report_error(result.error());
    auto invalid = profile;
    invalid.database = "intentionally_missing_database";
    weave::pg::Diagnostic diagnostic;
    auto failed = weave::pg::BlockingConnection::connect(std::move(invalid), diagnostic);
    if (failed || weave::pg::sqlstate(failed.error()) != "3D000" || diagnostic.sqlstate() != "3D000")
      return weave::report_error(std::make_error_code(std::errc::bad_message));

    if (auto result = ctx->run(probe(profile)); !result)
      return weave::report_error(result.error());
    if (auto result = ctx->run(fixture::pipeline_terminal(profile)); !result)
      return weave::report_error(result.error());
    if (auto result = ctx->run(weave::timeout(std::chrono::seconds{10}, fixture::replication(profile))); !result)
      return weave::report_error(result.error());
    if (auto result = ctx->run(
          weave::timeout(std::chrono::seconds{10}, fixture::replication_loaded(profile, certificates.directory)));
      !result) {
      return weave::report_error(result.error());
    }
    if (auto result = fixture::replication_blocking(profile); !result)
      return weave::report_error(result.error());
    std::fprintf(stderr, "Qualification %s: selection and reset\n", transport);
    if (auto result = ctx->run(connection_options(*ctx, profile, *standby_port, *client_address)); !result)
      return weave::report_error(result.error());
    if (auto result = ctx->run(balanced_connections(profile, *standby_port, *client_address)); !result)
      return weave::report_error(result.error());
    std::fprintf(stderr, "Qualification %s: blocking facade\n", transport);
    if (auto result = balanced_blocking(profile, *standby_port, *client_address); !result)
      return weave::report_error(result.error());
    if (auto result = blocking(profile, certificates.directory); !result)
      return weave::report_error(result.error());
  }

#if !defined(_WIN32)
  if (!local_directory.empty()) {
    auto local = plain;
    local.host = local_directory;
    local.hosts.clear();
    local.required_peer_user = geteuid();
    std::fprintf(stderr, "Qualification local: pipeline, COPY, functions, large objects, cancellation and reset\n");
    if (auto result = ctx->run(weave::timeout(std::chrono::seconds{20}, probe(local))); !result)
      return weave::report_error(result.error());
    if (auto result = blocking(local, certificates.directory); !result)
      return weave::report_error(result.error());

#if defined(WEAVE_TEST_RUNTIME)
    const std::array local_schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
    for (auto scheduler : local_schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
      if (!runtime)
        return weave::report_error(runtime.error());
      std::vector<weave::JoinHandle<void>> jobs;
      for (int index = 0; index < 16; ++index) {
        auto job = runtime->spawn(weave::timeout(std::chrono::seconds{20}, probe(local)));
        if (!job)
          return weave::report_error(job.error());
        jobs.push_back(std::move(*job));
      }
      for (auto &job : jobs) {
        if (auto result = std::move(job).get(); !result)
          return weave::report_error(result.error());
      }
    }
#endif
  }
#endif

  if (passthrough) {
    for (std::size_t index = 0; index < 8; ++index) {
      std::fprintf(stderr, "Qualification blocking key profile %zu\n", index);
      auto options = key_options(profiles[index % profiles.size()], *client_key, *server_key, index);
      if (auto result = blocking(options, certificates.directory); !result)
        return weave::report_error(result.error());
      if (auto result = ctx->run(weave::timeout(std::chrono::seconds{20}, probe(std::move(options)))); !result)
        return weave::report_error(result.error());
    }
  }

#ifdef WEAVE_TEST_RUNTIME
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto scheduler : schedulers) {
    const char *scheduler_name = scheduler == weave::Scheduler::worker_affine ? "affine" : "stealing";
    std::fprintf(stderr, "Qualification runtime %s: 64 clients\n", scheduler_name);
    auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler});
    if (!runtime)
      return weave::report_error(runtime.error());

    std::vector<weave::JoinHandle<void>> jobs;
    for (std::size_t index = 0; index < 64; ++index) {
      auto options = profiles[index % profiles.size()];
      if (passthrough)
        options = key_options(std::move(options), *client_key, *server_key, index);
      options.hosts = {{"localhost", *standby_port, *client_address}, {"localhost", *port, *client_address}};
      options.target_session = weave::pg::TargetSession::read_write;
      if (index % 4 >= 2)
        options.host_balance = weave::pg::HostBalance::random;
      if (index % 8 >= 4)
        options.max_protocol = weave::pg::ProtocolVersion::v30;
      auto task = weave::timeout(std::chrono::seconds{20}, probe(std::move(options), index));
      auto job = runtime->spawn(std::move(task).on_error([index](weave::Error error) noexcept {
        std::fprintf(stderr, "Runtime client %zu failed: %s\n", index, error.message().c_str());
      }));
      if (!job)
        return weave::report_error(job.error());

      jobs.push_back(std::move(*job));
    }

    for (auto &job : jobs) {
      if (auto result = std::move(job).get(); !result) {
        std::fprintf(stderr, "Qualification runtime %s: join failed\n", scheduler_name);
        return weave::report_error(result.error());
      }
    }

    jobs.clear();
    for (std::size_t index = 0; index < 8; ++index) {
      auto job = runtime->spawn(fixture::replication(profiles[index % profiles.size()]));
      if (!job)
        return weave::report_error(job.error());

      jobs.push_back(std::move(*job));
    }
    for (auto &job : jobs) {
      if (auto result = std::move(job).get(); !result)
        return weave::report_error(result.error());
    }
  }
#endif

  std::puts(
    "Native SCRAM, queries, pipelines, COPY BOTH/replication, cancellation and connection/reset qualification passed");
}
