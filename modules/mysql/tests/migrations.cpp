#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "migration_compiler.hpp"
#include <weave/database/detail/migration_plan.hpp>

namespace schema = weave::db::schema;
using weave::db::MigrationError;

TEST_CASE("MySQL identifiers and defaults are independent of sql_mode")
{
  CHECK(weave::mysql::detail::migration_identifier("a`b") == "`a``b`");
  CHECK(!weave::mysql::detail::migration_identifier(std::string(65, 'x')));
  CHECK(!weave::mysql::detail::migration_identifier(std::string_view{"a\0b", 3}));
  CHECK(!weave::mysql::detail::migration_identifier("trailing "));
  CHECK(!weave::mysql::detail::migration_identifier("\xc0\xaf"));
  CHECK(!weave::mysql::detail::migration_identifier("\xed\xa0\x80"));
  CHECK(!weave::mysql::detail::migration_identifier("\xf0\x9f\x98\x80"));
  CHECK(weave::mysql::detail::migration_identifier("\xc3\xa9"));
  CHECK(weave::mysql::detail::migration_utf8("\xf0\x9f\x98\x80"));
  CHECK(!weave::mysql::detail::migration_utf8("\xf5\x80\x80\x80"));
  CHECK(weave::mysql::detail::migration_literal("a'b\\c") == "CONVERT(X'6127625c63' USING utf8mb4)");
  CHECK(weave::mysql::detail::migration_literal("") == "CONVERT(X'' USING utf8mb4)");
}

TEST_CASE("MySQL compiler covers shared schema vocabulary without promising atomic DDL")
{
  weave::db::MigrationOptions options{.schema = "app", .history_schema = "app"};
  auto table = schema::create_table(
    "shipments",
    schema::identity("id").primary_key(),
    schema::varchar("name", 80).not_null().default_value("O'Brien"),
    schema::boolean("delivered").default_value(false),
    schema::decimal("weight", 12, 3),
    schema::timestamp("created"),
    schema::binary("data"));
  auto sql = weave::mysql::detail::compile_migration(table, options);
  REQUIRE(sql);
  if (!sql)
    return;
  CHECK(sql->find("`id` BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY") != std::string::npos);
  CHECK(sql->find("CHECK (`delivered` IN (0, 1))") != std::string::npos);
  CHECK(sql->find("DECIMAL(12,3)") != std::string::npos);
  CHECK(sql->find("DATETIME(6)") != std::string::npos);
  CHECK(sql->find("ENGINE=InnoDB") != std::string::npos);

  const std::array operations{
    schema::drop_table("shipments"),
    schema::rename_table("shipments", "packages"),
    schema::add_column("shipments", schema::date("date")),
    schema::drop_column("shipments", "date"),
    schema::rename_column("shipments", "name", "recipient"),
    schema::create_index("idx", "shipments", "name"),
    schema::drop_index("shipments", "idx")};
  for (const auto &operation : operations)
    CHECK(weave::mysql::detail::compile_migration(operation, options));

  CHECK(
    weave::mysql::detail::compile_migration(schema::drop_index("shipments", "idx"), options) ==
    "DROP INDEX `idx` ON `app`.`shipments`");
  CHECK(!weave::mysql::detail::compile_migration(schema::drop_index("idx"), options));
  CHECK(!weave::mysql::detail::compile_migration(weave::db::sql(weave::db::Vendor::postgres, "SELECT 1"), options));
}

TEST_CASE("MySQL compiler rejects malformed descriptions before changing schema")
{
  weave::db::MigrationOptions options{.schema = "app", .history_schema = "app"};
  const std::array invalid{
    schema::Operation{.object = "empty"},
    schema::create_table("dup", schema::int32("id"), schema::text("id")),
    schema::create_table("identity", schema::identity("id")),
    schema::create_table("keys", schema::int32("a").primary_key(), schema::int32("b").primary_key()),
    schema::add_column("t", schema::varchar("x", 0)),
    schema::add_column("t", schema::decimal("x", 66, 0)),
    schema::add_column("t", schema::decimal("x", 10, 11)),
    schema::add_column("t", schema::decimal("x", 40, 31)),
    schema::add_column("t", schema::boolean("x").default_value(1)),
    schema::add_column("t", schema::int16("x").default_value(32768)),
    schema::drop_table("weave_migrations"),
    schema::rename_table("t", "weave_migrations"),
    schema::create_index("idx", "t"),
    schema::create_index("idx", "t", "x", "x"),
    schema::create_table("t", schema::int32("x"), schema::foreign_key("missing").references("parent", "id"))};
  for (const auto &operation : invalid)
    CHECK(!weave::mysql::detail::compile_migration(operation, options));
}
