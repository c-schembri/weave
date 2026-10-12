#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <weave/database/migrations.hpp>
#include <weave/database/detail/migration_plan.hpp>
#include <weave/database/detail/migration_manifest.hpp>
#include <doctest/doctest.h>

namespace db = weave::db;
namespace schema = weave::db::schema;

template <class T>
static void expect_error(weave::Result<T> result, db::MigrationError error)
{
  CHECK_FALSE(result.has_value());
  if (!result)
    CHECK(result.error() == error);
}

namespace fixture {

struct Connection {};

template <class Range>
concept CanMigrate = requires(Connection &connection, Range &&range) {
  db::migrate(connection, std::forward<Range>(range));
  db::migration_status(connection, std::forward<Range>(range));
  db::rollback(connection, std::forward<Range>(range));
};

static auto manifest()
{
  return db::migrations(
    db::migration(1, "first", schema::create_table("shipments", schema::identity("id").primary_key()))
      .rollback(schema::drop_table("shipments")),
    db::migration(3, "third", schema::add_column("shipments", schema::text("name")))
      .rollback(schema::drop_column("shipments", "name")));
}

} // namespace fixture

namespace weave::db::detail {

template <>
struct MigrationDriver<fixture::Connection> {
  static Result<MigrationReport> migrate(fixture::Connection &, std::span<const Migration> entries, MigrationOptions)
  {
    return MigrationReport{.applied = entries.size()};
  }

  static Result<std::vector<MigrationStatus>> status(
    fixture::Connection &,
    std::span<const Migration>,
    MigrationOptions)
  {
    return std::vector<MigrationStatus>{};
  }

  static Result<MigrationReport> rollback(fixture::Connection &, std::span<const Migration>, RollbackOptions)
  {
    return MigrationReport{};
  }
};

} // namespace weave::db::detail

TEST_CASE("schema descriptions own names, literals and composite constraints")
{
  std::string recipient = "recipient";
  auto column = schema::text(recipient).not_null().default_value("O'Reilly");
  recipient = "changed";
  CHECK(column.name == "recipient");
  CHECK(!column.allows_null);
  REQUIRE(std::holds_alternative<std::string>(column.default_literal));
  if (!std::holds_alternative<std::string>(column.default_literal))
    return;
  CHECK(std::get<std::string>(column.default_literal) == "O'Reilly");

  auto table = schema::create_table(
    schema::qualified("app", "shipments"),
    schema::int64("tenant"),
    schema::int64("id"),
    schema::int64("customer"),
    schema::primary_key("tenant", "id"),
    schema::unique("tenant", "customer"),
    schema::foreign_key("tenant", "customer")
      .references("customers", "tenant", "id")
      .on_delete(schema::Action::cascade));
  CHECK(table.object.schema == "app");
  CHECK(table.columns.size() == 3);
  REQUIRE(table.constraints.size() == 3);
  if (table.constraints.size() != 3)
    return;
  CHECK(table.constraints[2].columns.size() == 2);
  CHECK(table.constraints[2].referenced_table.name == "customers");
  CHECK(table.constraints[2].delete_action == schema::Action::cascade);
}

TEST_CASE("migration manifest versions are positive, ascending and unique, with gaps allowed")
{
  auto entries = fixture::manifest();
  CHECK(db::detail::validate_manifest(entries).has_value());
  auto changed = entries;
  changed[1].version = 1;
  expect_error(db::detail::validate_manifest(changed), db::MigrationError::invalid_description);
  changed = entries;
  changed[0].version = 0;
  CHECK_FALSE(db::detail::validate_manifest(changed));
  changed = entries;
  changed[0].up.clear();
  CHECK_FALSE(db::detail::validate_manifest(changed));
  changed = entries;
  changed[0].name = std::string{"a\0b", 3};
  CHECK_FALSE(db::detail::validate_manifest(changed));
  changed = entries;
  changed[1].version = std::numeric_limits<weave::u64>::max();
  CHECK_FALSE(db::detail::validate_manifest(changed));
  CHECK(db::detail::validate_manifest({}).has_value());
}

TEST_CASE("canonical migration descriptions include directions, literals, policy and namespace")
{
  auto entries = fixture::manifest();
  const auto original = db::detail::canonical_migration(entries[0], "app");
  CHECK(original.starts_with("weave-migration-description:1;"));
  CHECK(original == db::detail::canonical_migration(entries[0], "app"));
  CHECK(original != db::detail::canonical_migration(entries[0], "other"));
  auto changed = entries[0];
  changed.down.clear();
  CHECK(original != db::detail::canonical_migration(changed, "app"));
  changed = entries[0];
  changed.transaction = db::MigrationTransaction::none;
  CHECK(original != db::detail::canonical_migration(changed, "app"));
  changed = entries[0];
  changed.up[0].columns[0].default_literal = weave::i64{42};
  CHECK(original != db::detail::canonical_migration(changed, "app"));
  auto a = db::migration(1, "ab", db::sql(db::Vendor::postgres, "c"));
  auto b = db::migration(1, "a", db::sql(db::Vendor::postgres, "bc"));
  CHECK(db::detail::canonical_migration(a, "app") != db::detail::canonical_migration(b, "app"));
}

TEST_CASE("history must be an exact clean prefix of the complete manifest")
{
  auto entries = fixture::manifest();
  const std::array<std::string, 2> checksums{"one", "three"};
  std::vector<db::detail::MigrationHistory> history{{1, "first", "one", "applied", "timestamp"}};
  CHECK(db::detail::validate_history(entries, history, checksums).has_value());
  history[0].checksum = "changed";
  expect_error(db::detail::validate_history(entries, history, checksums), db::MigrationError::history_mismatch);
  history[0].checksum = "one";
  history[0].state = "applying";
  expect_error(db::detail::validate_history(entries, history, checksums), db::MigrationError::dirty_history);
  history[0].state = "reverting";
  expect_error(db::detail::validate_history(entries, history, checksums), db::MigrationError::dirty_history);
  history[0].state = "applied";
  history[0].version = 3;
  expect_error(db::detail::validate_history(entries, history, checksums), db::MigrationError::history_mismatch);
  history[0].version = 1;
  history[0].applied_at.clear();
  CHECK_FALSE(db::detail::validate_history(entries, history, checksums));
}

TEST_CASE("forward targets cannot downgrade and reverse plans require every down operation")
{
  auto entries = fixture::manifest();
  auto range = db::detail::forward_migrations(entries, 1, std::nullopt);
  REQUIRE(range.has_value());
  if (!range)
    return;
  CHECK(range->first == 1);
  CHECK(range->last == 2);
  CHECK_FALSE(db::detail::forward_migrations(entries, 2, 1));
  CHECK_FALSE(db::detail::forward_migrations(entries, 0, 2));
  CHECK(db::detail::forward_migrations(entries, 0, 0).has_value());
  CHECK(db::detail::reverse_migrations(entries, 2, 2).has_value());
  CHECK_FALSE(db::detail::reverse_migrations(entries, 2, 3));
  entries[0].down.clear();
  CHECK(db::detail::reverse_migrations(entries, 2, 1).has_value());
  expect_error(db::detail::reverse_migrations(entries, 2, 2), db::MigrationError::missing_rollback);
}

TEST_CASE("borrowed manifests reject temporary owning ranges without requiring asynchronous drivers")
{
  CHECK(fixture::CanMigrate<std::array<db::Migration, 2> &>);
  CHECK(fixture::CanMigrate<std::vector<db::Migration> &>);
  CHECK(fixture::CanMigrate<std::span<const db::Migration>>);
  CHECK_FALSE(fixture::CanMigrate<std::array<db::Migration, 2>>);
  CHECK_FALSE(fixture::CanMigrate<std::vector<db::Migration>>);
  CHECK_FALSE(fixture::CanMigrate<std::initializer_list<db::Migration>>);
  fixture::Connection connection;
  auto entries = fixture::manifest();
  auto result = db::migrate(connection, entries);
  REQUIRE(result.has_value());
  if (result)
    CHECK(result->applied == 2);
}

TEST_CASE("generated manifest metadata mismatches become description errors")
{
  auto entry = fixture::manifest()[0];
  CHECK(db::detail::manifest_entry(entry, 1, "first").version == 1);
  auto wrong = db::migrations(db::detail::manifest_entry(entry, 2, "first"));
  CHECK_FALSE(db::detail::validate_manifest(wrong));
  wrong = db::migrations(db::detail::manifest_entry(entry, 1, "different"));
  CHECK_FALSE(db::detail::validate_manifest(wrong));
}

TEST_CASE("cleanup diagnostics distinguish no failure from a zero-valued error code")
{
  db::MigrationFailure failure;
  CHECK_FALSE(failure.cleanup_error.has_value());
  failure.cleanup_error = std::error_code{};
  CHECK(failure.cleanup_error.has_value());
}
