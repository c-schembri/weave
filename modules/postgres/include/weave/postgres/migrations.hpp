#pragma once

#include <weave/database/migrations.hpp>
#include <weave/postgres/connection.hpp>

namespace weave::db::detail {

/// PostgreSQL execution adapter for the shared schema descriptions; no libpq dependency.
template <>
struct MigrationDriver<pg::Connection> {
  static Task<MigrationReport> migrate(pg::Connection &, std::span<const Migration>, MigrationOptions);
  static Task<std::vector<MigrationStatus>> status(pg::Connection &, std::span<const Migration>, MigrationOptions);
  static Task<MigrationReport> rollback(pg::Connection &, std::span<const Migration>, RollbackOptions);
};

} // namespace weave::db::detail
