#include "package_migrations.hpp"
#include <weave/database/detail/migration_plan.hpp>

int main()
{
  auto entries = package_migrations();
  if (entries.size() != 3 || entries[0].version != 1 || entries[1].version != 2 || entries[2].version != 10)
    return 1;
  return weave::db::detail::validate_manifest(entries) ? 0 : 1;
}
