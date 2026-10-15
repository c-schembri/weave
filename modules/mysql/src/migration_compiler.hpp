#pragma once

#include <weave/database/migrations.hpp>

namespace weave::mysql::detail {

// Shared by compilation and execution; neither identifier nor value quoting depends on sql_mode.
Result<std::string> migration_identifier(std::string_view name);
bool migration_utf8(std::string_view value, bool identifier = false) noexcept;
std::string migration_literal(std::string_view value);
Result<std::string> compile_migration(const db::schema::Operation &operation, const db::MigrationOptions &options);

} // namespace weave::mysql::detail
