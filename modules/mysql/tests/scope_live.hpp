#pragma once

static weave::Task<void> lock_owner(weave::mysql::Connection &db, std::span<const weave::db::Migration> manifest)
{
  auto changed = co_await weave::db::migrate(db, manifest, {.history_table = "lock_history"});
  verify(changed.applied == 1, "migration lock owner");
}

static weave::Task<void> lock_contender(weave::mysql::Connection &db, std::span<const weave::db::Migration> manifest)
{
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  for (;;) {
    auto visible = co_await weave::as_result(db.execute("SELECT state FROM lock_history"));
    if (visible && !visible->rows.empty())
      break;
    verify(std::chrono::steady_clock::now() < deadline, "migration lock readiness");
    co_await weave::sleep_for(1ms);
  }
  auto locked = co_await weave::as_result(
    weave::db::migrate(db, manifest, {.history_table = "lock_history", .lock_timeout = 0ms}));
  verify(
    !locked && locked.error() == weave::db::MigrationError::lock_timeout && db.open(),
    "migration lock contention");
}

static weave::Task<void> qualify_scope(weave::mysql::Options options)
{
  auto db = co_await weave::mysql::connect(options);
  auto statement = co_await db.prepare(
    "SELECT CAST(? AS SIGNED) AS id, CAST(? AS CHAR) AS name, CAST(? AS DOUBLE) AS number, ? AS absent");
  const std::array<weave::mysql::ParameterView, 4> parameters{-42, "shipment", 12.75, std::nullopt};
  auto result = co_await statement.execute_borrowed(parameters);
  verify(result.rows.size() == 1, "prepared row");
  verify(result.rows[0]["id"].as<int>() == -42, "signed binary");
  verify(result.rows[0]["name"].as<std::string_view>() == "shipment", "binary string");
  verify(result.rows[0]["number"].as<double>() == 12.75, "binary float");
  verify(result.rows[0]["absent"].is_null() == true, "binary null");
  auto stream = co_await statement.stream_borrowed(parameters);
  verify(!db.close(), "stream owns busy lease");
  auto row = co_await stream.next();
  verify(row && (*row)["id"].as<int>() == -42, "prepared stream");
  verify(!(co_await stream.next()), "stream EOF");
  verify(stream.done(), "stream released busy");
  co_await statement.close();
  auto multiple = co_await db.query("SELECT 1 AS n; SELECT 2 AS n");
  verify(multiple.size() == 2 && multiple[1].rows[0]["n"].as<int>() == 2, "multiple results");
  auto rejected = co_await weave::as_result(db.execute("SELECT 1; SELECT 2"));
  verify(
    !rejected && rejected.error() == weave::mysql::ClientError::multiple_results && db.open(),
    "single drains extras");
  auto streamed = co_await db.stream("SELECT 1 AS n UNION ALL SELECT 2; SELECT 3 AS n");
  verify((co_await streamed.next())->operator[]("n").as<int>() == 1, "text stream first");
  co_await streamed.finish();
  verify(streamed.done() && db.open(), "stream drains multiple");
  std::vector<weave::mysql::Command> commands{
    {"SELECT 1 AS n"},
    {"SELECT missing_column"},
    {"SELECT CAST(? AS SIGNED) AS n", {7}}};
  auto batch = co_await db.batch(std::move(commands));
  verify(batch.size() == 3 && batch[1].failure && batch[2].results[0].rows[0]["n"].as<int>() == 7, "batch recovery");
  co_await db.execute("SET @weave_probe = 123");
  co_await db.reset();
  result = co_await db.execute("SELECT @weave_probe AS absent");
  verify(result.rows[0]["absent"].is_null() == true, "reset clears state");
  auto temporal = co_await db.prepare("SELECT CAST(? AS DATETIME(6)) AS datetime, CAST(? AS TIME(6)) AS time");
  std::vector<weave::mysql::Parameter> calendar{
    weave::mysql::DateTime{{2026, 10, 13}, 12, 34, 56, 123456},
    weave::mysql::Time{true, 2, 3, 4, 5, 234567}};
  result = co_await temporal.execute(std::move(calendar));
  verify(result.rows[0]["datetime"].datetime()->microsecond == 123456, "datetime");
  verify(result.rows[0]["time"].time()->days == 2 && result.rows[0]["time"].time()->negative, "time");
  co_await temporal.close();
  auto long_data = co_await db.prepare("SELECT OCTET_LENGTH(?) AS length");
  constexpr std::string_view first = "hello";
  co_await long_data.send_long_data(0, std::as_bytes(std::span{first}));
  co_await long_data.send_long_data(0, std::as_bytes(std::span{first}));
  const std::array<weave::mysql::ParameterView, 1> empty{std::string_view{}};
  result = co_await long_data.execute_borrowed(empty);
  verify(result.rows[0]["length"].as<int>() == 10, "long data");
  co_await long_data.close();

  auto packet_statement = co_await db.prepare("SELECT ? AS data");
  std::string payload(65536, 'x');
  payload.front() = 'a';
  payload[payload.size() / 2] = '\0';
  payload.back() = 'z';
  const std::array<weave::mysql::ParameterView, 1> packet_parameters{std::string_view{payload}};
  {
    auto echoed = co_await packet_statement.execute_borrowed(packet_parameters);
    verify(echoed.rows.size() == 1 && echoed.rows[0]["data"].bytes() == payload, "large prepared prefix send");
    auto retained = std::move(echoed.rows[0]);
    echoed.rows.clear();
    echoed.columns.clear();
    co_await db.execute("SELECT REPEAT('q', 65536) AS data");
    verify(retained["data"].bytes() == payload, "moved pooled row survives packet reuse and result mutation");
  }
  {
    auto stream = co_await packet_statement.stream_borrowed(packet_parameters);
    auto retained = co_await stream.next();
    verify(retained && (*retained)["data"].bytes() == payload, "large streamed row");
    verify(!(co_await stream.next()), "stream terminal packet");
    co_await db.execute("SELECT REPEAT('q', 65536) AS data");
    verify((*retained)["data"].bytes() == payload, "streamed row survives packet reuse");
  }
  co_await packet_statement.close();

  auto unsigned_statement = co_await db.prepare(
    "SELECT CAST(? AS UNSIGNED) AS maximum, CAST(? AS DECIMAL(20,6)) AS decimal_value");
  const std::array<weave::mysql::ParameterView, 2> exact_parameters{
    (std::numeric_limits<weave::u64>::max)(),
    weave::mysql::DecimalView{"123456789.123456"}};
  result = co_await unsigned_statement.execute_borrowed(exact_parameters);
  verify(
    result.rows[0]["maximum"].as<weave::u64>() == (std::numeric_limits<weave::u64>::max)() &&
      !result.rows[0]["maximum"].as<weave::i64>(),
    "unsigned binary range");
  verify(result.rows[0]["decimal_value"].as<weave::mysql::DecimalView>()->value == "123456789.123456", "exact decimal");
  co_await unsigned_statement.close();
  result = co_await db.execute(
    "SELECT CAST('2026-10-13 12:34:56.123' AS DATETIME(3)) AS datetime, CAST('-27:01:02.3' AS TIME(1)) AS time");
  verify(
    result.rows[0]["datetime"].datetime()->microsecond == 123000 &&
      result.rows[0]["time"].time()->microsecond == 300000,
    "text calendar precision");
  co_await db.execute("CREATE PROCEDURE scope_multi(IN id BIGINT) BEGIN SELECT id AS n; SELECT id+1 AS n; END");
  auto procedure = co_await db.prepare("CALL scope_multi(?)");
  const std::array<weave::mysql::ParameterView, 1> argument{7};
  auto procedure_results = co_await procedure.query_borrowed(argument);
  verify(
    procedure_results.size() == 3 && procedure_results[1].rows[0]["n"].as<int>() == 8,
    "prepared procedure multiple results");
  co_await procedure.close();
  auto pool = co_await weave::mysql::Pool::create(options, {.max_connections = 1});
  {
    auto lease = co_await pool.acquire();
    co_await lease->execute("SET @weave_probe = 999");
  }
  {
    auto lease = co_await pool.acquire();
    result = co_await lease->execute("SELECT @weave_probe AS absent");
    verify(result.rows[0]["absent"].is_null() == true, "pool resets");
  }
  verify(pool.stats().created == 1, "pool reuses");
  {
    auto lease = co_await pool.acquire();
    auto wait = co_await weave::as_result(weave::timeout(10ms, pool.acquire()));
    verify(!wait && wait.error() == std::errc::timed_out, "pool admission timeout");
    lease.discard();
  }
  {
    auto lease = co_await pool.acquire();
    verify(pool.stats().created == 2, "pool replacement");
  }
  namespace schema = weave::db::schema;
  const auto manifest = weave::db::migrations(
    weave::db::Migration{
      .version = 1,
      .name = "shipments",
      .up = {schema::create_table(
        "probe_shipments",
        schema::identity("id").primary_key(),
        schema::varchar("recipient", 80).not_null().default_value("a'b\\c"))},
      .down = {schema::drop_table("probe_shipments")},
      .transaction = weave::db::MigrationTransaction::none},
    weave::db::Migration{
      .version = 2,
      .name = "weight",
      .up =
        {schema::add_column("probe_shipments", schema::float64("weight")),
          schema::create_index("weight_index", "probe_shipments", "weight")},
      .down = {schema::drop_index("probe_shipments", "weight_index"), schema::drop_column("probe_shipments", "weight")},
      .transaction = weave::db::MigrationTransaction::none});
  auto report = co_await weave::db::migrate(db, manifest);
  verify(report.applied == 2 && report.version == 2, "migrations apply");
  auto history = co_await weave::db::migration_status(db, manifest);
  verify(history.size() == 2 && history[1].applied, "migrations status");
  auto folding = co_await db.execute("SELECT @@lower_case_table_names AS folding");
  if (folding.rows[0]["folding"].as<unsigned>().value_or(0)) {
    history = co_await weave::db::migration_status(db, manifest, {.history_table = "WEAVE_MIGRATIONS"});
    verify(history.size() == 2 && history[1].applied, "case-folded migration identity");
    const auto protected_history = weave::db::migrations(
      weave::db::Migration{
        .version = 1,
        .name = "reserved",
        .up = {schema::drop_table("WEAVE_MIGRATIONS")},
        .transaction = weave::db::MigrationTransaction::none});
    auto reserved = co_await weave::as_result(weave::db::migrate(db, protected_history));
    verify(
      !reserved && reserved.error() == weave::db::MigrationError::invalid_description,
      "case-folded journal protection");
  }
  const auto atomic = weave::db::migrations(
    weave::db::migration(1, "atomic", schema::create_table("atomic_rejected", schema::int32("id"))));
  auto unsupported = co_await weave::as_result(weave::db::migrate(db, atomic));
  verify(!unsupported && unsupported.error() == weave::db::MigrationError::unsupported, "reject atomic DDL guarantee");
  report = co_await weave::db::migrate(db, manifest);
  verify(report.applied == 0, "migrations repeat");
  report = co_await weave::db::rollback(db, manifest, {.steps = 2});
  verify(report.reverted == 2 && report.version == 0, "migrations rollback");
  const auto broken = weave::db::migrations(
    weave::db::Migration{
      .version = 1,
      .name = "broken",
      .up =
        {weave::db::sql(weave::db::Vendor::mysql, "CREATE TABLE probe_partial (id INT)"),
          weave::db::sql(weave::db::Vendor::mysql, "SELECT no_such_column")},
      .transaction = weave::db::MigrationTransaction::none});
  weave::db::MigrationFailure failure;
  auto failed = co_await weave::as_result(
    weave::db::migrate(db, broken, {.history_table = "probe_dirty", .failure = &failure}));
  verify(!failed && failure.version == 1 && db.open(), "migrations SQL failure");
  failed = co_await weave::as_result(weave::db::migrate(db, broken, {.history_table = "probe_dirty"}));
  verify(!failed && failed.error() == weave::db::MigrationError::dirty_history, "migrations dirty history");
  auto other = co_await weave::mysql::connect(options);
  const auto locking = weave::db::migrations(
    weave::db::Migration{
      .version = 1,
      .name = "lock_owner",
      .up = {weave::db::sql(weave::db::Vendor::mysql, "SELECT SLEEP(0.5)")},
      .transaction = weave::db::MigrationTransaction::none});
  co_await weave::when_all(lock_owner(db, locking), lock_contender(other, locking));
  co_await other.disconnect();
  const auto ownership = weave::db::migrations(
    weave::db::Migration{
      .version = 1,
      .name = "transaction_owner",
      .up = {weave::db::sql(weave::db::Vendor::mysql, "SET autocommit=0")},
      .transaction = weave::db::MigrationTransaction::none});
  failed = co_await weave::as_result(weave::db::migrate(db, ownership, {.history_table = "ownership_history"}));
  verify(
    !failed && failed.error() == weave::db::MigrationError::transaction_changed && !db.open(),
    "migration transaction ownership");
  pool.close();
  auto closed_pool = co_await weave::as_result(pool.acquire());
  verify(!closed_pool && closed_pool.error() == weave::mysql::ClientError::closed, "pool close admission");
}

#if defined(WEAVE_MYSQL_TEST_RUNTIME)
static weave::Task<void> pooled_worker(weave::mysql::Pool &pool, int id)
{
  for (int iteration = 0; iteration < 20; ++iteration) {
    auto lease = co_await pool.acquire();
    const std::array<weave::mysql::ParameterView, 1> parameters{id};
    auto result = co_await lease->execute_borrowed("SELECT CAST(? AS SIGNED) AS id, @previous AS absent", parameters);
    verify(
      result.rows[0]["id"].as<int>() == id && result.rows[0]["absent"].is_null() == true,
      "multicore pool isolation");
    co_await lease->execute("SET @previous=123");
  }
}

static weave::Task<void> qualify_multicore(weave::mysql::Options options)
{
  auto pool = co_await weave::mysql::Pool::create(std::move(options), {.max_connections = 2});
  weave::TaskScope scope;
  for (int id = 0; id < 8; ++id) {
    auto job = scope.spawn(pooled_worker(pool, id));
    if (!job) {
      scope.cancel();
      co_await scope.join();
      co_await weave::fail(job.error());
    }
  }
  co_await scope.join();
  verify(pool.stats().created == 2, "bounded multicore pool");
}
#endif

static void qualify_blocking(weave::mysql::Options options)
{
  auto blocking = weave::mysql::BlockingConnection::connect(options);
  verify(bool(blocking), "blocking connect");
  if (!blocking)
    return;
  auto statement = blocking->prepare("SELECT CAST(? AS SIGNED) AS id");
  verify(bool(statement), "blocking prepare");
  if (!statement)
    return;
  auto result = statement->execute({123});
  verify(result && result->rows[0]["id"].as<int>() == 123, "blocking prepared");
  verify(bool(statement->close()), "blocking close");
  auto stream = blocking->stream("SELECT 1 AS n UNION ALL SELECT 2");
  verify(stream && bool(stream->next()) && bool(stream->finish()), "blocking stream");
  namespace schema = weave::db::schema;
  const auto manifest = weave::db::migrations(
    weave::db::Migration{
      .version = 1,
      .name = "blocking",
      .up = {schema::create_table("blocking_migration", schema::int32("id"))},
      .down = {schema::drop_table("blocking_migration")},
      .transaction = weave::db::MigrationTransaction::none});
  auto migrated = weave::db::migrate(*blocking, manifest, {.history_table = "blocking_history"});
  verify(migrated && migrated->applied == 1, "blocking migration");
  auto reverted = weave::db::rollback(*blocking, manifest, {.history_table = "blocking_history"});
  verify(reverted && reverted->reverted == 1, "blocking rollback");
  auto status = weave::db::migration_status(*blocking, manifest, {.history_table = "blocking_history"});
  verify(status && status->size() == 1 && !(*status)[0].applied, "blocking migration status");
  verify(bool(blocking->disconnect()), "blocking disconnect");
}
