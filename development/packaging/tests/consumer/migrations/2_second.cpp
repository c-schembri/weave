#include <weave/database/migrations.hpp>

weave::db::Migration migration_2()
{
  return weave::db::migration(2, "second", weave::db::schema::drop_table("second"));
}
