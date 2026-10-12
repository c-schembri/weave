#include <weave/postgres/migrations.hpp>
#include <weave/database/detail/migration_plan.hpp>
#include <weave/timer.hpp>
#include "migration_access.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace weave::pg {

namespace {

namespace schema = db::schema;
using db::MigrationError;

std::error_code invalid()
{
  return db::make_error_code(MigrationError::invalid_description);
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

class Compiler {
  Connection &connection_;
  const db::MigrationOptions &options_;
  std::size_t identifier_limit_;
  std::optional<std::error_code> error_;

  std::string identifier(std::string_view name)
  {
    // PostgreSQL truncates overlong identifiers; accepting that would corrupt history identity and lock names.
    if (!db::detail::valid_migration_text(name) || name.size() > identifier_limit_) {
      error_ = invalid();
      return {};
    }
    auto escaped = connection_.escape_identifier(name);
    if (!escaped) {
      error_ = escaped.error();
      return {};
    }
    return std::move(*escaped);
  }

  std::string qualified(const schema::Name &name)
  {
    const auto &space = name.schema.empty() ? options_.schema : name.schema;
    if (space == "pg_temp" || space.starts_with("pg_temp_"))
      error_ = invalid();
    return identifier(space) + "." + identifier(name.name);
  }

  std::string literal(const schema::Literal &value)
  {
    return std::visit(
      [&]<class T>(const T &input) -> std::string {
        if constexpr (std::same_as<T, std::monostate>)
          return {};
        else if constexpr (std::same_as<T, std::string>) {
          auto escaped = connection_.escape_literal(input);
          if (!escaped) {
            error_ = escaped.error();
            return {};
          }
          return std::move(*escaped);
        } else if constexpr (std::same_as<T, bool>)
          return input ? "TRUE" : "FALSE";
        else {
          if constexpr (std::same_as<T, f64>) {
            if (!std::isfinite(input)) {
              error_ = invalid();
              return {};
            }
          }
          std::array<char, 128> buffer;
          auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), input);
          if (result.ec != std::errc{}) {
            error_ = invalid();
            return {};
          }
          return {buffer.data(), result.ptr};
        }
      },
      value);
  }

  bool default_matches(const schema::Column &column)
  {
    if (std::holds_alternative<std::monostate>(column.default_literal))
      return true;
    const auto type = column.type;
    if (type == schema::Type::boolean)
      return std::holds_alternative<bool>(column.default_literal);
    if (type == schema::Type::text || type == schema::Type::varchar || type == schema::Type::date ||
      type == schema::Type::timestamp || type == schema::Type::decimal)
      return std::holds_alternative<std::string>(column.default_literal);
    if (type == schema::Type::binary)
      return false;
    if (type == schema::Type::float32 || type == schema::Type::float64)
      return std::holds_alternative<f64>(column.default_literal) ||
        std::holds_alternative<i64>(column.default_literal) || std::holds_alternative<u64>(column.default_literal);

    const i64 limit = type == schema::Type::int16 ? std::numeric_limits<i16>::max()
      : type == schema::Type::int32               ? std::numeric_limits<i32>::max()
                                                  : std::numeric_limits<i64>::max();
    if (auto value = std::get_if<i64>(&column.default_literal))
      return *value >= -limit - 1 && *value <= limit;
    if (auto value = std::get_if<u64>(&column.default_literal))
      return *value <= static_cast<u64>(limit);
    return false;
  }

  std::string column(const schema::Column &value)
  {
    std::string type;
    switch (value.type) {
    case schema::Type::int16:
      type = "smallint";
      break;
    case schema::Type::int32:
      type = "integer";
      break;
    case schema::Type::int64:
      type = "bigint";
      break;
    case schema::Type::float32:
      type = "real";
      break;
    case schema::Type::float64:
      type = "double precision";
      break;
    case schema::Type::boolean:
      type = "boolean";
      break;
    case schema::Type::text:
      type = "text";
      break;
    case schema::Type::binary:
      type = "bytea";
      break;
    case schema::Type::date:
      type = "date";
      break;
    case schema::Type::timestamp:
      type = "timestamp without time zone";
      break;
    case schema::Type::varchar:
      if (value.length == 0 || value.length > 10485760 || value.scale != 0)
        error_ = invalid();
      type = "varchar(" + std::to_string(value.length) + ")";
      break;
    case schema::Type::decimal:
      if (value.length == 0 || value.length > 1000 || value.scale > value.length)
        error_ = invalid();
      type = "numeric(" + std::to_string(value.length) + "," + std::to_string(value.scale) + ")";
      break;
    default:
      error_ = db::make_error_code(MigrationError::unsupported);
      break;
    }
    if (value.type != schema::Type::decimal && value.type != schema::Type::varchar && (value.length || value.scale))
      error_ = invalid();
    if (value.identity &&
      (value.type != schema::Type::int64 || value.allows_null || value.default_literal.index() != 0))
      error_ = invalid();
    if (!default_matches(value) || (value.primary && value.allows_null))
      error_ = invalid();

    std::string result = identifier(value.name) + " " + type;
    if (value.identity)
      result += " GENERATED BY DEFAULT AS IDENTITY";
    if (!value.allows_null)
      result += " NOT NULL";
    if (value.default_literal.index() != 0)
      result += " DEFAULT " + literal(value.default_literal);
    if (value.primary)
      result += " PRIMARY KEY";
    if (value.unique_value)
      result += " UNIQUE";
    return result;
  }

  std::string identifiers(const std::vector<std::string> &names)
  {
    if (names.empty())
      error_ = invalid();
    std::string result;
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (std::find(names.begin(), names.begin() + index, names[index]) != names.begin() + index)
        error_ = invalid();
      if (index)
        result += ", ";
      result += identifier(names[index]);
    }
    return result;
  }

  std::string action(schema::Action value)
  {
    switch (value) {
    case schema::Action::no_action:
      return "NO ACTION";
    case schema::Action::restrict:
      return "RESTRICT";
    case schema::Action::cascade:
      return "CASCADE";
    case schema::Action::set_null:
      return "SET NULL";
    }
    error_ = invalid();
    return {};
  }

  std::string constraint(const schema::Constraint &value, const std::vector<schema::Column> &columns)
  {
    for (const auto &name : value.columns) {
      if (std::none_of(columns.begin(), columns.end(), [&](const auto &column) {
            return column.name == name;
          }))
        error_ = invalid();
    }
    std::string result;
    switch (value.kind) {
    case schema::Constraint::Kind::primary_key:
      result = "PRIMARY KEY";
      break;
    case schema::Constraint::Kind::unique:
      result = "UNIQUE";
      break;
    case schema::Constraint::Kind::foreign_key:
      result = "FOREIGN KEY";
      break;
    default:
      error_ = invalid();
      break;
    }
    result += " (" + identifiers(value.columns) + ")";
    if (value.kind == schema::Constraint::Kind::foreign_key) {
      if (value.columns.size() != value.referenced_columns.size())
        error_ = invalid();
      result += " REFERENCES " + qualified(value.referenced_table) + " (" + identifiers(value.referenced_columns) + ")";
      result += " ON DELETE " + action(value.delete_action) + " ON UPDATE " + action(value.update_action);
    } else if (!value.referenced_table.name.empty() || !value.referenced_table.schema.empty() ||
      !value.referenced_columns.empty() || value.delete_action != schema::Action::no_action ||
      value.update_action != schema::Action::no_action)
      error_ = invalid();
    return result;
  }

public:
  Compiler(Connection &connection, const db::MigrationOptions &options, std::size_t identifier_limit)
      : connection_(connection), options_(options), identifier_limit_(identifier_limit)
  {
  }

  Result<std::string> name(const schema::Name &value)
  {
    auto result = qualified(value);
    if (error_)
      return std::unexpected(*error_);
    return result;
  }

  Result<std::string> compile(const schema::Operation &operation)
  {
    error_.reset();
    const auto kind = operation.kind;
    if (kind == schema::Operation::Kind::sql) {
      if (operation.vendor != db::Vendor::postgres)
        return std::unexpected(db::make_error_code(MigrationError::unsupported));
      if (!db::detail::valid_migration_text(operation.statement) || !operation.object.name.empty() ||
        !operation.object.schema.empty() || !operation.new_name.empty() || !operation.columns.empty() ||
        !operation.constraints.empty() || !operation.index_columns.empty() || operation.unique_index)
        return std::unexpected(invalid());
      return operation.statement;
    }

    const auto &space = operation.object.schema.empty() ? options_.schema : operation.object.schema;
    if (space == options_.history_schema && operation.object.name == options_.history_table)
      return std::unexpected(invalid());
    if (!operation.statement.empty() || (kind != schema::Operation::Kind::create_index && operation.unique_index))
      return std::unexpected(invalid());
    if (kind != schema::Operation::Kind::create_table && !operation.constraints.empty())
      return std::unexpected(invalid());
    if (kind != schema::Operation::Kind::create_table && kind != schema::Operation::Kind::add_column &&
      !operation.columns.empty())
      return std::unexpected(invalid());
    if (kind != schema::Operation::Kind::create_index && kind != schema::Operation::Kind::rename_column &&
      !operation.index_columns.empty())
      return std::unexpected(invalid());
    const bool replacement = kind == schema::Operation::Kind::rename_table ||
      kind == schema::Operation::Kind::drop_column || kind == schema::Operation::Kind::rename_column ||
      kind == schema::Operation::Kind::create_index;
    if (!replacement && !operation.new_name.empty())
      return std::unexpected(invalid());

    const auto object = qualified(operation.object);
    std::string result;
    switch (kind) {
    case schema::Operation::Kind::create_table: {
      if (operation.columns.empty())
        error_ = invalid();
      result = "CREATE TABLE " + object + " (";
      std::size_t primary_keys = 0;
      for (std::size_t index = 0; index < operation.columns.size(); ++index) {
        const auto &value = operation.columns[index];
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
          if (operation.columns[earlier].name == value.name)
            error_ = invalid();
        }
        primary_keys += value.primary;
        if (index)
          result += ", ";
        result += column(value);
      }
      for (const auto &value : operation.constraints) {
        primary_keys += value.kind == schema::Constraint::Kind::primary_key;
        result += ", " + constraint(value, operation.columns);
      }
      if (primary_keys > 1)
        error_ = invalid();
      result += ")";
      break;
    }
    case schema::Operation::Kind::drop_table:
      result = "DROP TABLE " + object;
      break;
    case schema::Operation::Kind::rename_table:
      if (space == options_.history_schema && operation.new_name == options_.history_table)
        error_ = invalid();
      result = "ALTER TABLE " + object + " RENAME TO " + identifier(operation.new_name);
      break;
    case schema::Operation::Kind::add_column:
      if (operation.columns.size() != 1)
        error_ = invalid();
      else
        result = "ALTER TABLE " + object + " ADD COLUMN " + column(operation.columns.front());
      break;
    case schema::Operation::Kind::drop_column:
      result = "ALTER TABLE " + object + " DROP COLUMN " + identifier(operation.new_name);
      break;
    case schema::Operation::Kind::rename_column:
      if (operation.index_columns.size() != 1)
        error_ = invalid();
      else
        result = "ALTER TABLE " + object + " RENAME COLUMN " + identifier(operation.index_columns.front()) + " TO " +
          identifier(operation.new_name);
      break;
    case schema::Operation::Kind::create_index:
      result = operation.unique_index ? "CREATE UNIQUE INDEX " : "CREATE INDEX ";
      result += identifier(operation.new_name) + " ON " + object + " (" + identifiers(operation.index_columns) + ")";
      break;
    case schema::Operation::Kind::drop_index:
      result = "DROP INDEX " + object;
      break;
    default:
      error_ = db::make_error_code(MigrationError::unsupported);
      break;
    }
    if (error_)
      return std::unexpected(*error_);
    return result;
  }
};

struct Plan {
  std::vector<std::string> up;
  std::vector<std::string> down;
};

enum class Mode {
  apply,
  status,
  rollback
};

struct RunState {
  Connection &connection;
  db::MigrationOptions options;
  db::MigrationFailure failure;
  std::string table;
  std::string lock_arguments;
  bool locked = false;
  bool transaction_owned = false;
  std::size_t identifier_limit = 63;
  std::vector<Plan> plans;
  std::vector<std::string> checksums;
  std::vector<db::detail::MigrationHistory> history;
};

struct RunResult {
  db::MigrationReport report;
  std::vector<db::MigrationStatus> status;
};

void phase(RunState &state, db::MigrationPhase value, u64 version = 0)
{
  state.failure.phase = value;
  state.failure.version = version;
}

Task<void> command(RunState &state, std::string sql, std::vector<Parameter> parameters = {})
{
  co_await state.connection.execute(std::move(sql), std::move(parameters));
}

Result<void> validate_session(RunState &state)
{
  if (!state.connection.open() || state.connection.transaction() != Transaction::idle ||
    state.connection.pipeline_status() != PipelineStatus::off)
    return std::unexpected(db::make_error_code(MigrationError::invalid_session));
  auto encoding = state.connection.client_encoding();
  if (!encoding || *encoding != Encoding::utf8)
    return std::unexpected(db::make_error_code(MigrationError::unsupported));
  auto version = detail::MigrationAccess::server_version(state.connection);
  if (!version)
    return std::unexpected(version.error());
  if (*version < 100000 || state.options.lock_timeout.count() < 0 ||
    state.options.lock_timeout > std::chrono::hours{24})
    return std::unexpected(invalid());

  return {};
}

Task<void> resolve_namespaces(RunState &state)
{
  auto settings = co_await state.connection.execute(
    "SELECT pg_catalog.current_schema(),pg_catalog.current_setting('max_identifier_length')");
  if (settings.rows.size() != 1 || settings.rows.front().size() != 2)
    co_await fail(Error::protocol);
  const auto &row = settings.rows.front();
  if (state.options.schema.empty()) {
    if (row[0].is_null())
      co_await fail(invalid());
    state.options.schema = row[0].bytes();
  }
  if (state.options.history_schema.empty())
    state.options.history_schema = state.options.schema;
  auto limit = row[1].as<u32>();
  if (!limit || *limit == 0)
    co_await fail(Error::protocol);
  state.identifier_limit = *limit;
}

Result<void> preflight(RunState &state, std::span<const db::Migration> manifest)
{
  auto valid = db::detail::validate_manifest(manifest);
  if (!valid)
    return valid;
  Compiler compiler{state.connection, state.options, state.identifier_limit};
  auto table = compiler.name(schema::qualified(state.options.history_schema, state.options.history_table));
  auto target_schema = compiler.name(schema::qualified(state.options.schema, "_"));
  if (!table || !target_schema)
    return std::unexpected(!table ? table.error() : target_schema.error());
  state.table = std::move(*table);
  auto lock = digest("weave:migrations:1:" + state.table);
  if (!lock)
    return std::unexpected(lock.error());
  u32 first = 0, second = 0;
  auto a = std::from_chars(lock->data(), lock->data() + 8, first, 16);
  auto b = std::from_chars(lock->data() + 8, lock->data() + 16, second, 16);
  if (a.ec != std::errc{} || b.ec != std::errc{})
    return std::unexpected(invalid());
  state.lock_arguments = std::to_string(std::bit_cast<i32>(first)) + "," + std::to_string(std::bit_cast<i32>(second));

  state.plans.reserve(manifest.size());
  state.checksums.reserve(manifest.size());
  for (const auto &entry : manifest) {
    state.failure.version = entry.version;
    Plan plan;
    for (const auto &operation : entry.up) {
      auto statement = compiler.compile(operation);
      if (!statement)
        return std::unexpected(statement.error());
      plan.up.push_back(std::move(*statement));
    }
    for (const auto &operation : entry.down) {
      auto statement = compiler.compile(operation);
      if (!statement)
        return std::unexpected(statement.error());
      plan.down.push_back(std::move(*statement));
    }
    auto checksum = digest(db::detail::canonical_migration(entry, state.options.schema));
    if (!checksum)
      return std::unexpected(checksum.error());
    state.checksums.push_back(std::move(*checksum));
    state.plans.push_back(std::move(plan));
  }
  return {};
}

Task<void> acquire_lock(RunState &state)
{
  phase(state, db::MigrationPhase::lock);
  auto deadline = std::chrono::steady_clock::now() + state.options.lock_timeout;
  for (;;) {
    auto result = co_await state.connection.execute(
      "SELECT pg_catalog.pg_try_advisory_lock(" + state.lock_arguments + ")");
    if (result.rows.size() != 1 || result.rows.front().size() != 1)
      co_await fail(Error::protocol);
    auto acquired = result.rows.front().front().as<bool>();
    if (!acquired)
      co_await fail(acquired.error());
    if (*acquired) {
      state.locked = true;
      co_return;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      co_await fail(db::make_error_code(MigrationError::lock_timeout));
    co_await sleep_until(std::min(deadline, now + std::chrono::milliseconds{25}));
  }
}

Task<bool> history_exists(RunState &state)
{
  auto result = co_await state.connection.execute(
    "SELECT relpersistence,relkind FROM pg_catalog.pg_class WHERE oid=pg_catalog.to_regclass($1)",
    {{state.table}});
  if (result.rows.empty())
    co_return false;
  if (result.rows.size() != 1 || result.rows.front().size() != 2)
    co_await fail(Error::protocol);
  const auto &row = result.rows.front();
  if (row[0].bytes() != "p" || row[1].bytes() != "r")
    co_await fail(db::make_error_code(MigrationError::history_mismatch));
  co_return true;
}

Task<void> read_history(RunState &state)
{
  auto result = co_await state.connection.execute(
    "SELECT version,name,checksum,state,applied_at::text,description_format FROM " + state.table + " ORDER BY version");
  for (const auto &row : result.rows) {
    if (row.size() != 6)
      co_await fail(Error::protocol);
    auto version = row[0].as<u64>();
    auto format = row[5].as<i32>();
    if (!version || !format || *format != 1 || row[1].is_null() || row[2].is_null() || row[3].is_null())
      co_await fail(db::make_error_code(MigrationError::history_mismatch));
    state.history.push_back(
      {*version,
        std::string{row[1].bytes()},
        std::string{row[2].bytes()},
        std::string{row[3].bytes()},
        row[4].is_null() ? std::string{} : std::string{row[4].bytes()}});
  }
}

Task<void> begin(RunState &state)
{
  state.transaction_owned = true;
  co_await command(state, "BEGIN");
}

Task<void> commit(RunState &state, u64 version)
{
  phase(state, db::MigrationPhase::commit, version);
  co_await command(state, "COMMIT");
  state.transaction_owned = false;
  if (state.connection.transaction() != Transaction::idle)
    co_await fail(db::make_error_code(MigrationError::transaction_changed));
}

Task<void> apply_one(RunState &state, const db::Migration &entry, std::size_t index, bool reverting)
{
  const bool atomic = entry.transaction == db::MigrationTransaction::required;
  phase(state, reverting ? db::MigrationPhase::rollback : db::MigrationPhase::apply, entry.version);
  const auto version = std::to_string(entry.version);
  if (atomic)
    co_await begin(state);
  else if (reverting)
    co_await command(state, "UPDATE " + state.table + " SET state='reverting' WHERE version=$1::bigint", {{version}});
  else
    co_await command(
      state,
      "INSERT INTO " + state.table +
        " (version,name,checksum,state,description_format) VALUES ($1::bigint,$2,$3,'applying',1)",
      {{version}, {entry.name}, {state.checksums[index]}});

  const auto &statements = reverting ? state.plans[index].down : state.plans[index].up;
  for (const auto &statement : statements) {
    co_await command(state, statement);
    const auto expected = atomic ? Transaction::active : Transaction::idle;
    if (state.connection.transaction() != expected) {
      static_cast<void>(state.connection.close());
      co_await fail(db::make_error_code(MigrationError::transaction_changed));
    }
  }

  if (reverting)
    co_await command(state, "DELETE FROM " + state.table + " WHERE version=$1::bigint", {{version}});
  else if (atomic)
    co_await command(
      state,
      "INSERT INTO " + state.table +
        " (version,name,checksum,state,applied_at,description_format)"
        " VALUES ($1::bigint,$2,$3,'applied',pg_catalog.clock_timestamp(),1)",
      {{version}, {entry.name}, {state.checksums[index]}});
  else
    co_await command(
      state,
      "UPDATE " + state.table + " SET state='applied',applied_at=pg_catalog.clock_timestamp() WHERE version=$1::bigint",
      {{version}});
  if (atomic)
    co_await commit(state, entry.version);
}

Task<RunResult> execute(RunState &state, std::span<const db::Migration> manifest, Mode mode, std::size_t steps)
{
  auto session = validate_session(state);
  if (!session)
    co_await fail(session.error());
  co_await resolve_namespaces(state);
  auto valid = preflight(state, manifest);
  if (!valid)
    co_await fail(valid.error());
  auto target = db::detail::migration_target(manifest, state.options.target);
  if (!target)
    co_await fail(target.error());
  co_await acquire_lock(state);
  phase(state, db::MigrationPhase::history);
  const bool exists = co_await history_exists(state);
  if (exists)
    co_await read_history(state);
  auto history = db::detail::validate_history(manifest, state.history, state.checksums);
  if (!history) {
    for (std::size_t index = 0; index < state.history.size(); ++index) {
      const auto &stored = state.history[index];
      if (index >= manifest.size() || stored.version != manifest[index].version ||
        stored.name != manifest[index].name || stored.checksum != state.checksums[index] || stored.state != "applied" ||
        stored.applied_at.empty()) {
        state.failure.version = stored.version;
        break;
      }
    }
    co_await fail(history.error());
  }
  RunResult result;
  result.report.version = state.history.empty() ? 0 : state.history.back().version;
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
    if (!range) {
      phase(state, db::MigrationPhase::validation);
      if (range.error() == db::MigrationError::missing_rollback) {
        for (std::size_t index = state.history.size() - steps; index < state.history.size(); ++index) {
          if (manifest[index].down.empty()) {
            state.failure.version = manifest[index].version;
            break;
          }
        }
      }
      co_await fail(range.error());
    }
    for (std::size_t index = range->last; index > range->first; --index) {
      co_await apply_one(state, manifest[index - 1], index - 1, true);
      ++result.report.reverted;
    }
    result.report.version = range->first == 0 ? 0 : manifest[range->first - 1].version;
    co_return result;
  }

  auto range = db::detail::forward_migrations(manifest, state.history.size(), state.options.target);
  if (!range)
    co_await fail(range.error());
  if (range->first == range->last)
    co_return result;
  if (!exists) {
    co_await begin(state);
    co_await command(
      state,
      "CREATE TABLE " + state.table +
        " (version bigint PRIMARY KEY CHECK(version>0),name text NOT NULL,checksum text NOT NULL,"
        "state text NOT NULL CHECK(state IN ('applying','applied','reverting')),"
        "applied_at timestamp with time zone,description_format integer NOT NULL CHECK(description_format=1))");
    co_await commit(state, 0);
  }
  for (std::size_t index = range->first; index < range->last; ++index) {
    co_await apply_one(state, manifest[index], index, false);
    ++result.report.applied;
    result.report.version = manifest[index].version;
  }
  co_return result;
}

Task<void> cleanup(RunState &state)
{
  if (!state.connection.open())
    co_return;
  if (state.transaction_owned) {
    const auto transaction = state.connection.transaction();
    if (transaction == Transaction::active || transaction == Transaction::failed)
      co_await command(state, "ROLLBACK");
    else if (transaction != Transaction::idle)
      co_await fail(db::make_error_code(MigrationError::invalid_session));
    state.transaction_owned = false;
  }
  if (state.locked) {
    auto result = co_await state.connection.execute(
      "SELECT pg_catalog.pg_advisory_unlock(" + state.lock_arguments + ")");
    if (result.rows.size() != 1 || result.rows.front().size() != 1)
      co_await fail(Error::protocol);
    auto released = result.rows.front().front().as<bool>();
    if (!released || !*released)
      co_await fail(db::make_error_code(MigrationError::invalid_session));
    state.locked = false;
  }
}

Task<RunResult> run(
  Connection &connection,
  std::span<const db::Migration> manifest,
  db::MigrationOptions options,
  Mode mode,
  std::size_t steps,
  [[maybe_unused]] detail::MigrationAccess::Borrow borrow)
{
  RunState state{connection, std::move(options)};
  if (state.options.failure)
    *state.options.failure = {};

  auto result = co_await as_result(execute(state, manifest, mode, steps));
  if (!result) {
    state.failure.error = result.error();
    auto diagnostic = detail::MigrationAccess::failure(connection);
    if (diagnostic && diagnostic->error == result.error())
      state.failure.message = diagnostic->diagnostic.message();
    if (state.failure.message.empty())
      state.failure.message = result.error().message();
  }

  // The failed child is drained before cleanup. Parent cancellation cannot skip rollback/unlock.
  auto operation = cleanup(state);
  weave::detail::TaskAccess::bind(operation, {});
  auto cleaned = co_await as_result(std::move(operation));
  if (!cleaned) {
    state.failure.cleanup_error = cleaned.error();
    static_cast<void>(connection.close());
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

Task<db::MigrationReport> report(Task<RunResult> operation)
{
  auto result = co_await std::move(operation);
  co_return result.report;
}

Task<std::vector<db::MigrationStatus>> statuses(Task<RunResult> operation)
{
  auto result = co_await std::move(operation);
  co_return std::move(result.status);
}

} // namespace

} // namespace weave::pg

namespace weave::db::detail {

Task<MigrationReport> MigrationDriver<pg::Connection>::migrate(
  pg::Connection &connection,
  std::span<const Migration> manifest,
  MigrationOptions options)
{
  return pg::report(
    pg::run(
      connection,
      manifest,
      std::move(options),
      pg::Mode::apply,
      0,
      pg::detail::MigrationAccess::borrow(connection)));
}

Task<std::vector<MigrationStatus>> MigrationDriver<pg::Connection>::status(
  pg::Connection &connection,
  std::span<const Migration> manifest,
  MigrationOptions options)
{
  return pg::statuses(
    pg::run(
      connection,
      manifest,
      std::move(options),
      pg::Mode::status,
      0,
      pg::detail::MigrationAccess::borrow(connection)));
}

Task<MigrationReport> MigrationDriver<pg::Connection>::rollback(
  pg::Connection &connection,
  std::span<const Migration> manifest,
  RollbackOptions options)
{
  MigrationOptions policy{
    .schema = std::move(options.schema),
    .history_schema = std::move(options.history_schema),
    .history_table = std::move(options.history_table),
    .lock_timeout = options.lock_timeout,
    .failure = options.failure};
  return pg::report(
    pg::run(
      connection,
      manifest,
      std::move(policy),
      pg::Mode::rollback,
      options.steps,
      pg::detail::MigrationAccess::borrow(connection)));
}

} // namespace weave::db::detail
