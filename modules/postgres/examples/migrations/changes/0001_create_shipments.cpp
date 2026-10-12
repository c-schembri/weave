#include <weave/database/migrations.hpp>

namespace db = weave::db;
namespace schema = weave::db::schema;

db::Migration migration_0001()
{
  return db::migration(
    1,
    "create_shipments",
    schema::create_table(
      "shipments",
      schema::identity("id").primary_key(),
      schema::text("recipient").not_null(),
      schema::boolean("delivered").not_null().default_value(false)))
    .rollback(schema::drop_table("shipments"));
}
