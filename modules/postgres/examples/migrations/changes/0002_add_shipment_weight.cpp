#include <weave/database/migrations.hpp>

namespace db = weave::db;
namespace schema = weave::db::schema;

db::Migration migration_0002()
{
  return db::migration(2, "add_shipment_weight", schema::add_column("shipments", schema::float64("weight").nullable()))
    .rollback(schema::drop_column("shipments", "weight"));
}
