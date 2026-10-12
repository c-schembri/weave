#include <weave/database/migrations.hpp>

weave::db::Migration migration_001()
{
  return weave::db::migration(1, "first", weave::db::schema::drop_table("first"));
}
