#include <weave/database/migrations.hpp>

weave::db::Migration migration_10()
{
  return weave::db::migration(10, "tenth", weave::db::schema::drop_table("tenth"));
}
