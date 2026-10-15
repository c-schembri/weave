#include <weave/database/migrations.hpp>

weave::db::Migration migration_0001()
{
  namespace schema = weave::db::schema;
  return {
    .version = 1,
    .name = "shipments",
    .up = {schema::create_table(
      "shipments",
      schema::identity("id").primary_key(),
      schema::varchar("recipient", 80).not_null())},
    .down = {schema::drop_table("shipments")},
    .transaction = weave::db::MigrationTransaction::none};
}
