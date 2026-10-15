#include <weave/mysql/migrations.hpp>
#include <weave/database/detail/migration_plan.hpp>
#include <weave/io.hpp>
#include <weave/timer.hpp>
#include "migration_access.hpp"
#include "migration_compiler.hpp"
#include <openssl/evp.h>

namespace weave::mysql {

namespace {

using Access = detail::MigrationAccess;
using db::MigrationError;

std::error_code migration_error(MigrationError value)
{
  return db::make_error_code(value);
}

Result<std::string> digest(std::string_view input)
{
  std::array<unsigned char, 32> bytes;
  unsigned size = 0;
  if (EVP_Digest(input.data(), input.size(), bytes.data(), &size, EVP_sha256(), nullptr) != 1 || size != bytes.size())
    return std::unexpected(std::make_error_code(std::errc::io_error));
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (auto byte : bytes) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}

enum class Mode {
  apply,
  status,
  rollback
};

struct Plan {
  std::vector<std::string> up;
  std::vector<std::string> down;
};

struct RunResult {
  db::MigrationReport report;
  std::vector<db::MigrationStatus> status;
};

struct RunState {
  Access::Lease lease;
  db::MigrationOptions options;
  db::MigrationFailure failure;
  std::vector<Plan> plans;
  std::vector<std::string> checksums;
  std::vector<db::detail::MigrationHistory> history;
  std::string table;
  std::string lock;
  bool locked = false;
  bool history_exists = false;
  bool folded = false;
};

void phase(RunState &state, db::MigrationPhase value, u64 version = 0)
{
  state.failure.phase = value;
  state.failure.version = version;
}

template <class T>
Result<T> scalar(const ResultSet &result, std::string_view name)
{
  if (result.rows.size() != 1)
    return std::unexpected(make_error_code(ClientError::protocol));
  return result.rows[0][name].template as<T>();
}

Task<void> command(RunState &state, std::string sql)
{
  co_await Access::execute(state.lease, std::move(sql));
  if (!Access::idle(state.lease))
    co_await fail(migration_error(MigrationError::transaction_changed));
}

Task<void> ownership(RunState &state)
{
  if (!Access::idle(state.lease))
    co_await fail(migration_error(MigrationError::transaction_changed));
  auto result = co_await Access::execute(
    state.lease,
    "SELECT IS_USED_LOCK(" + state.lock + ") = CONNECTION_ID() AS owned");
  auto owned = scalar<bool>(result, "owned");
  if (!owned || !*owned)
    co_await fail(migration_error(MigrationError::invalid_session));
}

Task<void> initialize(RunState &state, std::span<const db::Migration> manifest)
{
  if (auto valid = db::detail::validate_manifest(manifest); !valid)
    co_await fail(valid.error());
  if (state.options.lock_timeout < std::chrono::milliseconds::zero() ||
    state.options.lock_timeout > std::chrono::hours{24})
    co_await fail(migration_error(MigrationError::invalid_description));
  for (const auto &entry : manifest) {
    if (!detail::migration_utf8(entry.name))
      co_await fail(migration_error(MigrationError::invalid_description));
    if (entry.transaction != db::MigrationTransaction::none)
      co_await fail(migration_error(MigrationError::unsupported));
  }
  if (auto ready = state.lease.acquire(); !ready)
    co_await fail(ready.error());
  auto current = co_await Access::execute(
    state.lease,
    "SELECT DATABASE() AS database_name, @@lower_case_table_names AS folded");
  auto folding = scalar<unsigned>(current, "folded");
  if (!folding)
    co_await fail(folding.error());
  state.folded = *folding != 0;
  if (state.options.schema.empty()) {
    auto name = scalar<std::string>(current, "database_name");
    if (!name)
      co_await fail(migration_error(MigrationError::invalid_description));
    state.options.schema = std::move(*name);
  }
  if (state.options.history_schema.empty())
    state.options.history_schema = state.options.schema;

  auto space = detail::migration_identifier(state.options.schema);
  auto history_space = detail::migration_identifier(state.options.history_schema);
  auto table = detail::migration_identifier(state.options.history_table);
  if (!space || !history_space || !table)
    co_await fail(migration_error(MigrationError::invalid_description));
  state.table = *history_space + "." + *table;
  for (const auto &entry : manifest) {
    Plan plan;
    const std::array directions{std::pair{&entry.up, &plan.up}, std::pair{&entry.down, &plan.down}};
    for (const auto &[operations, statements] : directions) {
      for (const auto &operation : *operations) {
        if (state.folded && operation.kind != db::schema::Operation::Kind::sql) {
          const auto &space_name = operation.object.schema.empty() ? state.options.schema : operation.object.schema;
          const auto replacement = operation.kind == db::schema::Operation::Kind::rename_table ? operation.new_name
                                                                                               : operation.object.name;
          auto protected_name = co_await Access::execute(
            state.lease,
            "SELECT (BINARY LOWER(" + detail::migration_literal(space_name) + ") = BINARY LOWER(" +
              detail::migration_literal(state.options.history_schema) + ")) AND ((BINARY LOWER(" +
              detail::migration_literal(operation.object.name) + ") = BINARY LOWER(" +
              detail::migration_literal(state.options.history_table) + ")) OR (BINARY LOWER(" +
              detail::migration_literal(replacement) + ") = BINARY LOWER(" +
              detail::migration_literal(state.options.history_table) + "))) AS reserved");
          auto reserved = scalar<bool>(protected_name, "reserved");
          if (!reserved || *reserved)
            co_await fail(migration_error(MigrationError::invalid_description));
        }
        auto sql = detail::compile_migration(operation, state.options);
        if (!sql)
          co_await fail(sql.error());
        statements->push_back(std::move(*sql));
      }
    }
    auto checksum = digest(db::detail::canonical_migration(entry, state.options.schema));
    if (!checksum)
      co_await fail(checksum.error());
    state.checksums.push_back(std::move(*checksum));
    state.plans.push_back(std::move(plan));
  }

  // Match the server's identifier folding, not a locale-dependent client-side approximation.
  std::string lock_space = state.options.history_schema;
  std::string lock_table = state.options.history_table;
  if (state.folded) {
    auto normalized = co_await Access::execute(
      state.lease,
      "SELECT LOWER(" + detail::migration_literal(lock_space) + ") AS space, LOWER(" +
        detail::migration_literal(lock_table) + ") AS name");
    auto normalized_space = scalar<std::string>(normalized, "space");
    auto normalized_table = scalar<std::string>(normalized, "name");
    if (!normalized_space || !normalized_table)
      co_await fail(make_error_code(ClientError::protocol));
    lock_space = std::move(*normalized_space);
    lock_table = std::move(*normalized_table);
  }
  db::detail::MigrationEncoding identity;
  identity.text("weave.mysql.migration-lock:1");
  identity.text(lock_space);
  identity.text(lock_table);
  auto key = digest(std::move(identity).finish());
  if (!key)
    co_await fail(key.error());
  state.lock = detail::migration_literal(*key);
  phase(state, db::MigrationPhase::lock);
  auto existing = co_await Access::execute(
    state.lease,
    "SELECT COALESCE(IS_USED_LOCK(" + state.lock + ") = CONNECTION_ID(), 0) AS owned");
  auto owned = scalar<bool>(existing, "owned");
  if (!owned || *owned)
    co_await fail(migration_error(MigrationError::invalid_session));
  const auto timeout = std::to_string(state.options.lock_timeout.count() / 1000.0);
  auto acquired = co_await Access::execute(
    state.lease,
    "SELECT GET_LOCK(" + state.lock + "," + timeout + ") AS acquired");
  auto locked = scalar<int>(acquired, "acquired");
  if (!locked)
    co_await fail(migration_error(MigrationError::invalid_session));
  if (*locked != 1)
    co_await fail(migration_error(MigrationError::lock_timeout));
  state.locked = true;
}

Task<void> read_history(RunState &state)
{
  phase(state, db::MigrationPhase::history);
  const auto space_name = detail::migration_literal(state.options.history_schema);
  const auto table_name = detail::migration_literal(state.options.history_table);
  const auto space_comparison = state.folded ? "LOWER(TABLE_SCHEMA) = LOWER(" + space_name + ")"
                                             : "TABLE_SCHEMA = " + space_name;
  const auto table_comparison = state.folded ? "LOWER(TABLE_NAME) = LOWER(" + table_name + ")"
                                             : "TABLE_NAME = " + table_name;
  auto present = co_await Access::execute(
    state.lease,
    "SELECT ENGINE AS engine, TABLE_TYPE AS kind FROM information_schema.tables WHERE " +
      (state.folded ? space_comparison : "BINARY TABLE_SCHEMA = BINARY " + space_name) + " AND " +
      (state.folded ? table_comparison : "BINARY TABLE_NAME = BINARY " + table_name));
  if (present.rows.empty())
    co_return;
  auto engine = scalar<std::string_view>(present, "engine");
  auto kind = scalar<std::string_view>(present, "kind");
  if (!engine || !kind || *engine != "InnoDB" || *kind != "BASE TABLE")
    co_await fail(migration_error(MigrationError::history_mismatch));
  state.history_exists = true;
  auto stored = co_await Access::execute(
    state.lease,
    "SELECT version,name,checksum,state,CAST(applied_at AS CHAR) AS applied_at,description_format FROM " + state.table +
      " ORDER BY version");
  for (const auto &row : stored.rows) {
    auto version = row["version"].as<u64>();
    auto name = row["name"].as<std::string>();
    auto checksum = row["checksum"].as<std::string>();
    auto status = row["state"].as<std::string>();
    auto timestamp = row["applied_at"].as<std::optional<std::string>>();
    auto format = row["description_format"].as<unsigned>();
    if (!version || !name || !checksum || !status || !timestamp || !format || *format != 1)
      co_await fail(migration_error(MigrationError::history_mismatch));
    state.history.push_back(
      {*version, std::move(*name), std::move(*checksum), std::move(*status), timestamp->value_or("")});
  }
}

Task<void> change(RunState &state, const db::Migration &entry, std::size_t index, bool reverting)
{
  phase(state, reverting ? db::MigrationPhase::rollback : db::MigrationPhase::apply, entry.version);
  co_await ownership(state);
  const auto version = std::to_string(entry.version);
  if (reverting) {
    auto marked = co_await Access::execute(
      state.lease,
      "UPDATE " + state.table + " SET state='reverting' WHERE version=" + version + " AND state='applied'");
    if (marked.affected_rows != 1)
      co_await fail(migration_error(MigrationError::history_mismatch));
  } else {
    co_await command(
      state,
      "INSERT INTO " + state.table + " (version,name,checksum,state,description_format) VALUES (" + version + "," +
        detail::migration_literal(entry.name) + "," + detail::migration_literal(state.checksums[index]) +
        ", 'applying',1)");
  }
  const auto &operations = reverting ? state.plans[index].down : state.plans[index].up;
  for (const auto &sql : operations) {
    co_await command(state, sql);
    co_await ownership(state);
  }
  phase(state, db::MigrationPhase::commit, entry.version);
  if (reverting) {
    auto erased = co_await Access::execute(
      state.lease,
      "DELETE FROM " + state.table + " WHERE version=" + version + " AND state='reverting'");
    if (erased.affected_rows != 1)
      co_await fail(migration_error(MigrationError::history_mismatch));
  } else {
    auto completed = co_await Access::execute(
      state.lease,
      "UPDATE " + state.table + " SET state='applied',applied_at=UTC_TIMESTAMP(6) WHERE version=" + version +
        " AND state='applying'");
    if (completed.affected_rows != 1)
      co_await fail(migration_error(MigrationError::history_mismatch));
  }
}

Task<RunResult> execute(RunState &state, std::span<const db::Migration> manifest, Mode mode, std::size_t steps)
{
  co_await initialize(state, manifest);
  co_await read_history(state);
  if (auto valid = db::detail::validate_history(manifest, state.history, state.checksums); !valid)
    co_await fail(valid.error());
  RunResult result;
  if (!state.history.empty())
    result.report.version = state.history.back().version;
  if (mode == Mode::status) {
    for (std::size_t index = 0; index < manifest.size(); ++index) {
      const bool applied = index < state.history.size();
      result.status.push_back(
        {manifest[index].version,
          manifest[index].name,
          applied,
          applied ? state.history[index].applied_at : std::string{}});
    }
    co_return result;
  }
  if (mode == Mode::rollback) {
    auto range = db::detail::reverse_migrations(manifest, state.history.size(), steps);
    if (!range)
      co_await fail(range.error());
    for (std::size_t end = range->last; end > range->first; --end) {
      co_await change(state, manifest[end - 1], end - 1, true);
      ++result.report.reverted;
      result.report.version = end > 1 ? manifest[end - 2].version : 0;
    }
    co_return result;
  }
  auto range = db::detail::forward_migrations(manifest, state.history.size(), state.options.target);
  if (!range)
    co_await fail(range.error());
  if (range->first == range->last)
    co_return result;
  if (!state.history_exists) {
    co_await command(
      state,
      "CREATE TABLE " + state.table +
        " (version BIGINT PRIMARY KEY CHECK(version>0),name LONGTEXT NOT NULL,checksum CHAR(64) CHARACTER SET ascii "
        "COLLATE ascii_bin NOT NULL,"
        "state ENUM('applying','applied','reverting') NOT NULL,applied_at DATETIME(6),"
        "description_format INT NOT NULL CHECK(description_format=1)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    state.history_exists = true;
  }
  for (std::size_t index = range->first; index < range->last; ++index) {
    co_await change(state, manifest[index], index, false);
    ++result.report.applied;
    result.report.version = manifest[index].version;
  }
  co_return result;
}

Task<void> cleanup(RunState &state)
{
  if (!state.lease.connection->open() || !state.locked)
    co_return;
  if (!Access::idle(state.lease)) {
    Access::close(state.lease);
    co_await fail(migration_error(MigrationError::transaction_changed));
  }
  auto released = co_await Access::execute(state.lease, "SELECT RELEASE_LOCK(" + state.lock + ") AS released");
  auto value = scalar<int>(released, "released");
  if (!value || *value != 1)
    co_await fail(migration_error(MigrationError::invalid_session));
  state.locked = false;
}

Task<RunResult> run(
  Access::Lease lease,
  std::span<const db::Migration> manifest,
  db::MigrationOptions options,
  Mode mode,
  std::size_t steps)
{
  RunState state{std::move(lease), std::move(options)};
  if (state.options.failure)
    *state.options.failure = {};
  auto result = co_await as_result(execute(state, manifest, mode, steps));
  if (!result) {
    state.failure.error = result.error();
    auto diagnostic = state.lease.connection->last_failure();
    if (diagnostic && *diagnostic && server_error((*diagnostic)->number) == result.error())
      state.failure.message = (*diagnostic)->message;
    if (state.failure.message.empty())
      state.failure.message = result.error().message();
  }
  auto operation = timeout(std::chrono::seconds{5}, cleanup(state));
  weave::detail::TaskAccess::bind(operation, {});
  auto cleaned = co_await as_result(std::move(operation));
  if (!cleaned) {
    state.failure.cleanup_error = cleaned.error();
    Access::close(state.lease);
    if (result) {
      state.failure.phase = db::MigrationPhase::cleanup;
      state.failure.error = cleaned.error();
      state.failure.message = cleaned.error().message();
      result = std::unexpected(cleaned.error());
    }
  }
  if (state.options.failure && !result)
    *state.options.failure = std::move(state.failure);
  if (!result)
    co_await fail(result.error());
  co_return std::move(*result);
}

Task<db::MigrationReport> report(Task<RunResult> task)
{
  auto result = co_await std::move(task);
  co_return result.report;
}

Task<std::vector<db::MigrationStatus>> statuses(Task<RunResult> task)
{
  auto result = co_await std::move(task);
  co_return std::move(result.status);
}

} // namespace

} // namespace weave::mysql

namespace weave::db::detail {

Task<MigrationReport> MigrationDriver<mysql::Connection>::migrate(
  mysql::Connection &connection,
  std::span<const Migration> manifest,
  MigrationOptions options)
{
  return mysql::report(
    mysql::run(
      mysql::detail::MigrationAccess::borrow(connection),
      manifest,
      std::move(options),
      mysql::Mode::apply,
      0));
}

Task<std::vector<MigrationStatus>> MigrationDriver<mysql::Connection>::status(
  mysql::Connection &connection,
  std::span<const Migration> manifest,
  MigrationOptions options)
{
  return mysql::statuses(
    mysql::run(
      mysql::detail::MigrationAccess::borrow(connection),
      manifest,
      std::move(options),
      mysql::Mode::status,
      0));
}

Task<MigrationReport> MigrationDriver<mysql::Connection>::rollback(
  mysql::Connection &connection,
  std::span<const Migration> manifest,
  RollbackOptions options)
{
  const auto steps = options.steps;
  MigrationOptions policy{
    .schema = std::move(options.schema),
    .history_schema = std::move(options.history_schema),
    .history_table = std::move(options.history_table),
    .lock_timeout = options.lock_timeout,
    .failure = options.failure};
  return mysql::report(
    mysql::run(
      mysql::detail::MigrationAccess::borrow(connection),
      manifest,
      std::move(policy),
      mysql::Mode::rollback,
      steps));
}

Result<MigrationReport> MigrationDriver<mysql::BlockingConnection>::migrate(
  mysql::BlockingConnection &connection,
  std::span<const Migration> manifest,
  MigrationOptions options)
{
  return mysql::detail::MigrationAccess::context(connection)
    .run(db::migrate(mysql::detail::MigrationAccess::session(connection), manifest, std::move(options)));
}

Result<std::vector<MigrationStatus>> MigrationDriver<mysql::BlockingConnection>::status(
  mysql::BlockingConnection &connection,
  std::span<const Migration> manifest,
  MigrationOptions options)
{
  return mysql::detail::MigrationAccess::context(connection)
    .run(db::migration_status(mysql::detail::MigrationAccess::session(connection), manifest, std::move(options)));
}

Result<MigrationReport> MigrationDriver<mysql::BlockingConnection>::rollback(
  mysql::BlockingConnection &connection,
  std::span<const Migration> manifest,
  RollbackOptions options)
{
  return mysql::detail::MigrationAccess::context(connection)
    .run(db::rollback(mysql::detail::MigrationAccess::session(connection), manifest, std::move(options)));
}

} // namespace weave::db::detail
