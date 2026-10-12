#include <weave/postgres.hpp>
#include <weave/io.hpp>
#include <weave/port.hpp>
#include <weave/log.hpp>
#include "tls_certificates.hpp"
#ifdef WEAVE_POSTGRES_TEST_RUNTIME
#include "runtime_fixture.hpp"
#endif
#include <iostream>
#include <cstdio>
#include <cstdlib>
#include <source_location>

namespace db = weave::db;
namespace pg = weave::pg;
namespace schema = weave::db::schema;

static void check(bool value, std::source_location location = std::source_location::current())
{
  if (!value) {
    std::fprintf(stderr, "Migration check failed: %s:%u\n", location.file_name(), location.line());
    std::exit(EXIT_FAILURE);
  }
}

static weave::Task<void> apply_concurrent(
  pg::Connection &connection,
  std::span<const db::Migration> manifest,
  std::size_t &applied)
{
  auto report = co_await db::migrate(connection, manifest, {.history_table = "migration_parallel"});
  applied = report.applied;
}

static weave::Task<void> lock_checks(
  pg::Connection &observer,
  weave::u32 owner,
  std::span<const db::Migration> manifest)
{
  for (;;) {
    auto locks = co_await observer.execute(
      "SELECT EXISTS(SELECT 1 FROM pg_locks WHERE locktype='advisory' AND pid=" + std::to_string(owner) + ")");
    if (locks.rows.front().front().bytes() == "t")
      break;
    co_await weave::sleep_for(std::chrono::milliseconds{1});
  }
  db::MigrationFailure failure;
  auto timed = co_await weave::as_result(
    db::migrate(
      observer,
      manifest,
      {.history_table = "migration_parallel", .lock_timeout = std::chrono::milliseconds{5}, .failure = &failure}));
  check(!timed && timed.error() == db::MigrationError::lock_timeout && failure.phase == db::MigrationPhase::lock);
  auto stopped = co_await weave::as_result(
    weave::timeout(
      std::chrono::milliseconds{5},
      db::migrate(observer, manifest, {.history_table = "migration_parallel"})));
  check(!stopped && stopped.error() == std::errc::timed_out);
  // A deadline can arrive during a query rather than a timer, legitimately retiring the observer session.
  if (observer.open())
    co_await observer.execute("SELECT pg_advisory_unlock(81234567::bigint)");
}

static weave::Task<void> exercise(pg::Options options)
{
  auto connection = co_await pg::connect(options);
  auto manifest = db::migrations(
    db::migration(
      1,
      "create_shipments",
      schema::create_table(
        "migration_shipments",
        schema::identity("id").primary_key(),
        schema::text("recipient").not_null().default_value("O'Reilly"),
        schema::boolean("delivered").not_null().default_value(false)))
      .rollback(schema::drop_table("migration_shipments")),
    db::migration(2, "add_weight", schema::add_column("migration_shipments", schema::float64("weight")))
      .rollback(schema::drop_column("migration_shipments", "weight")));
  auto status = co_await db::migration_status(connection, manifest);
  check(status.size() == 2 && !status[0].applied);
  auto first = co_await db::migrate(connection, manifest, {.target = 1});
  check(first.applied == 1 && first.version == 1);
  auto second = co_await db::migrate(connection, manifest);
  check(second.applied == 1 && second.version == 2);
  auto repeat = co_await db::migrate(connection, manifest);
  check(repeat.applied == 0 && repeat.version == 2);
  auto concurrent_index = db::migrations(
    manifest[0],
    manifest[1],
    db::migration(
      3,
      "concurrent_index",
      db::sql(db::Vendor::postgres, "CREATE INDEX CONCURRENTLY migration_weight_index ON migration_shipments(weight)"))
      .rollback(db::sql(db::Vendor::postgres, "DROP INDEX CONCURRENTLY migration_weight_index"))
      .nontransactional());
  auto indexed = co_await db::migrate(connection, concurrent_index);
  check(indexed.applied == 1 && indexed.version == 3);
  auto unindexed = co_await db::rollback(connection, concurrent_index);
  check(unindexed.reverted == 1 && unindexed.version == 2);
  co_await connection.execute("INSERT INTO migration_shipments DEFAULT VALUES");
  auto row = co_await connection.execute("SELECT recipient, delivered FROM migration_shipments");
  check(row.rows.front()[0].bytes() == "O'Reilly" && row.rows.front()[1].bytes() == "f");
  auto old = co_await weave::as_result(db::migrate(connection, manifest, {.target = 1}));
  check(!old && old.error() == db::make_error_code(db::MigrationError::invalid_target));
  auto changed = manifest;
  changed[0].name = "modified";
  auto mismatch = co_await weave::as_result(db::migrate(connection, changed));
  check(!mismatch && mismatch.error() == db::make_error_code(db::MigrationError::history_mismatch));
  auto rolled = co_await db::rollback(connection, manifest);
  check(rolled.reverted == 1 && rolled.version == 1);
  auto all = co_await db::rollback(connection, manifest);
  check(all.reverted == 1 && all.version == 0);
  auto absent = co_await connection.execute("SELECT to_regclass('migration_shipments') IS NULL");
  check(absent.rows.front().front().bytes() == "t");

  db::MigrationFailure failure;
  auto atomic = db::migrations(
    db::migration(
      1,
      "atomic_failure",
      schema::create_table("migration_atomic", schema::int32("id")),
      db::sql(db::Vendor::postgres, "SELECT nonexistent_migration_column")));
  auto failed = co_await weave::as_result(db::migrate(connection, atomic, {.failure = &failure}));
  check(!failed && failure.version == 1 && failure.phase == db::MigrationPhase::apply && !failure.cleanup_error);
  check(failure.message.find("nonexistent_migration_column") != std::string::npos);
  check(connection.transaction() == pg::Transaction::idle);
  auto undone = co_await connection.execute("SELECT to_regclass('migration_atomic') IS NULL");
  check(undone.rows.front().front().bytes() == "t");
  auto empty = co_await connection.execute("SELECT count(*) FROM weave_migrations");
  check(empty.rows.front().front().bytes() == "0");
  auto nonatomic = atomic;
  nonatomic[0].transaction = db::MigrationTransaction::none;
  auto partial = co_await weave::as_result(db::migrate(connection, nonatomic));
  check(!partial);
  auto dirty = co_await weave::as_result(db::migration_status(connection, nonatomic));
  check(!dirty && dirty.error() == db::make_error_code(db::MigrationError::dirty_history));
  co_await connection.execute("DROP TABLE migration_atomic");
  co_await connection.execute("DROP TABLE weave_migrations");

  auto unsupported = db::migrations(db::migration(1, "wrong_vendor", db::sql(db::Vendor::mysql, "SELECT 1")));
  auto rejected = co_await weave::as_result(db::migrate(connection, unsupported));
  check(!rejected && rejected.error() == db::make_error_code(db::MigrationError::unsupported));
  auto untouched = co_await connection.execute("SELECT to_regclass('weave_migrations') IS NULL");
  check(untouched.rows.front().front().bytes() == "t");

  auto cancelled = db::migrations(
    db::migration(
      1,
      "cancelled",
      schema::create_table("migration_cancelled", schema::int32("id")),
      db::sql(db::Vendor::postgres, "SELECT pg_sleep(2)")));
  auto stopped = co_await weave::as_result(
    weave::timeout(std::chrono::milliseconds{100}, db::migrate(connection, cancelled)));
  check(!stopped && stopped.error() == std::errc::timed_out);
  auto next = co_await pg::connect(options);
  auto retry = co_await db::migration_status(next, cancelled);
  check(retry.size() == 1 && !retry[0].applied);
  auto gone = co_await next.execute("SELECT to_regclass('migration_cancelled') IS NULL");
  check(gone.rows.front().front().bytes() == "t");
  co_await next.execute("DROP TABLE IF EXISTS weave_migrations");

  auto other = co_await pg::connect(options);
  auto parallel = db::migrations(
    db::migration(
      1,
      "parallel",
      schema::create_table("migration_parallel_data", schema::int64("id")),
      db::sql(db::Vendor::postgres, "SELECT pg_advisory_xact_lock(81234567::bigint)")));
  co_await other.execute("SELECT pg_advisory_lock(81234567::bigint)");
  std::size_t winner = 0;
  co_await weave::when_all(
    apply_concurrent(next, parallel, winner),
    lock_checks(other, next.backend_process(), parallel));
  check(winner == 1);
  auto resumed = co_await pg::connect(options);
  auto ready = co_await db::migrate(resumed, parallel, {.history_table = "migration_parallel"});
  check(ready.applied == 0);
  co_await next.execute("DROP TABLE migration_parallel_data");
  co_await next.execute("DROP TABLE migration_parallel");

  std::size_t a = 0, b = 0;
  co_await weave::when_all(apply_concurrent(next, parallel, a), apply_concurrent(resumed, parallel, b));
  check(a + b == 1);
  co_await next.execute("DROP TABLE migration_parallel_data");
  co_await next.execute("DROP TABLE migration_parallel");

  auto no_down = db::migrations(
    db::migration(1, "irreversible", schema::create_table("migration_no_down", schema::int32("id"))),
    db::migration(2, "reversible", schema::create_table("migration_has_down", schema::int32("id")))
      .rollback(schema::drop_table("migration_has_down")));
  co_await db::migrate(next, no_down, {.history_table = "migration_down_history"});
  auto missing = co_await weave::as_result(
    db::rollback(next, no_down, {.steps = 2, .history_table = "migration_down_history", .failure = &failure}));
  check(!missing && missing.error() == db::MigrationError::missing_rollback && failure.version == 1);
  auto kept = co_await next.execute("SELECT to_regclass('migration_has_down') IS NOT NULL");
  check(kept.rows.front().front().bytes() == "t");
  co_await next.execute("DROP TABLE migration_has_down");
  co_await next.execute("DROP TABLE migration_no_down");
  co_await next.execute("DROP TABLE migration_down_history");

  co_await next.execute("BEGIN");
  auto active = co_await weave::as_result(db::migration_status(next, manifest));
  check(
    !active && active.error() == db::MigrationError::invalid_session && next.transaction() == pg::Transaction::active);
  co_await next.execute("ROLLBACK");
  auto temporary = co_await weave::as_result(db::migrate(next, manifest, {.schema = "pg_temp"}));
  check(!temporary && temporary.error() == db::MigrationError::invalid_description);
  co_await next.execute("CREATE UNLOGGED TABLE migration_unlogged(x integer)");
  auto unlogged = co_await weave::as_result(
    db::migration_status(next, manifest, {.history_table = "migration_unlogged"}));
  check(!unlogged && unlogged.error() == db::MigrationError::history_mismatch);
  co_await next.execute("DROP TABLE migration_unlogged");
}

static weave::Task<void> schema_operations(pg::Options options)
{
  auto connection = co_await pg::connect(std::move(options));
  auto manifest = db::migrations(
    db::migration(
      1,
      "schema_operations",
      schema::create_table(
        "migration_types",
        schema::int32("tenant").not_null().default_value(0),
        schema::identity("id"),
        schema::int16("small").default_value(-32768),
        schema::int64("large").default_value(weave::i64{9223372036854775807}),
        schema::float32("ratio").default_value(1.5f),
        schema::float64("weight").default_value(2.25),
        schema::varchar("recipient", 80).not_null().default_value("O'Reilly"),
        schema::binary("data"),
        schema::date("day").default_value("2026-10-11"),
        schema::timestamp("created").default_value("2026-10-11 12:00:00"),
        schema::decimal("cost", 9, 2).default_value("12.34"),
        schema::text("odd\"name").default_value("quoted"),
        schema::primary_key("tenant", "id"),
        schema::unique("recipient", "tenant")),
      schema::create_table(
        "migration_children",
        schema::int32("tenant"),
        schema::int64("parent"),
        schema::foreign_key("tenant", "parent")
          .references("migration_types", "tenant", "id")
          .on_delete(schema::Action::cascade)
          .on_update(schema::Action::cascade)),
      schema::create_index("migration_type_index", "migration_types", "tenant", "id").unique(),
      schema::rename_column("migration_types", "created", "created_at"),
      schema::rename_table("migration_types", "migration_types_done"),
      schema::drop_index("migration_type_index"),
      schema::drop_column("migration_types_done", "data"))
      .rollback(schema::drop_table("migration_children"), schema::drop_table("migration_types_done")));
  co_await db::migrate(connection, manifest, {.history_table = "migration_types_history"});
  co_await connection.execute("INSERT INTO migration_types_done DEFAULT VALUES");
  auto row = co_await connection.execute(
    "SELECT small,large,ratio,weight,recipient,cost,\"odd\"\"name\" FROM migration_types_done");
  check(row.rows.size() == 1 && row.rows.front()[0].bytes() == "-32768");
  check(row.rows.front()[1].bytes() == "9223372036854775807");
  check(row.rows.front()[4].bytes() == "O'Reilly" && row.rows.front()[5].bytes() == "12.34");
  auto reversed = co_await db::rollback(connection, manifest, {.history_table = "migration_types_history"});
  check(reversed.reverted == 1);
  co_await connection.execute("DROP TABLE migration_types_history");
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
  pg::Options options{
    .host = "127.0.0.1",
    .port = *port,
    .user = "administrator",
    .database = "postgres",
    .password = password,
    .plaintext = true};
  options.max_protocol = pg::ProtocolVersion::v30;
  auto ctx = weave::Context::create();
  check(ctx.has_value());
  auto result = ctx->run(exercise(options));
  if (!result)
    return weave::report_error(result.error());
  auto descriptions = ctx->run(schema_operations(options));
  if (!descriptions)
    return weave::report_error(descriptions.error());
  auto secured = options;
  weave::TlsClientOptions policy;
  policy.ca_file = files.ca;
  policy.certificate_file = files.client;
  policy.private_key_file = files.client_key;
  auto credentials = weave::TlsContext::client(policy);
  check(credentials.has_value());
  secured.tls = std::move(*credentials);
  secured.plaintext = false;
  auto encrypted = ctx->run(exercise(secured));
  if (!encrypted)
    return weave::report_error(encrypted.error());

#ifdef WEAVE_POSTGRES_TEST_RUNTIME
  const std::array schedulers{weave::Scheduler::worker_affine, weave::Scheduler::work_stealing};
  for (auto layout : support::io_layouts) {
    for (auto scheduler : schedulers) {
      auto runtime = weave::Runtime::create({.workers = 4, .scheduler = scheduler, .io_layout = layout});
      check(runtime.has_value());
      auto executed = runtime->run(exercise(secured));
      if (!executed)
        return weave::report_error(executed.error());
    }
  }
#endif

  std::puts("Migrations: atomicity, history, rollback, concurrent runners, deadlines and cancellation passed");
}
