#include <weave/database/migrations.hpp>

namespace db = weave::db;
namespace schema = weave::db::schema;

db::Migration migration_0003()
{
  return {
    .version = 3,
    .name = "add_shipment_tracking",

    .up =
      {
        schema::add_column("shipments", schema::text("tracking_code").nullable()),
        schema::create_index("shipments_tracking_idx", "shipments", "tracking_code"),
      },

    .down =
      {
        schema::drop_index("shipments_tracking_idx"),
        schema::drop_column("shipments", "tracking_code"),
      },
  };
}
