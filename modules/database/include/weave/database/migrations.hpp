#pragma once

#include <weave/database/schema.hpp>
#include <weave/task.hpp>
#include <array>
#include <span>
#include <chrono>
#include <optional>
#include <ranges>

namespace weave::db {

/// Required means atomic schema/history changes per migration; unsupported guarantees are errors.
enum class MigrationTransaction {
  required,
  none
};
/// Migration runner failures; SQL/transport failures preserve the driver's original error code.
enum class MigrationError {
  invalid_description = 1,
  unsupported,
  history_mismatch,
  dirty_history,
  missing_rollback,
  invalid_target,
  lock_timeout,
  invalid_session,
  transaction_changed
};

struct Migration;

namespace detail {

class MigrationCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.migrations";
  }

  std::string message(int value) const override
  {
    switch (static_cast<MigrationError>(value)) {
    case MigrationError::invalid_description:
      return "Invalid migration description";
    case MigrationError::unsupported:
      return "Unsupported migration operation or guarantee";
    case MigrationError::history_mismatch:
      return "Migration history does not match the manifest";
    case MigrationError::dirty_history:
      return "Interrupted nontransactional migration requires manual recovery";
    case MigrationError::missing_rollback:
      return "Migration has no explicit rollback operations";
    case MigrationError::invalid_target:
      return "Invalid migration target or rollback count";
    case MigrationError::lock_timeout:
      return "Migration lock deadline expired";
    case MigrationError::invalid_session:
      return "Migrations require an idle dedicated session";
    case MigrationError::transaction_changed:
      return "Migration SQL changed transaction ownership";
    }
    return "Unknown migration error";
  }
};

template <class Connection>
struct MigrationDriver;

template <class Range>
concept TemporaryMigrationCollection = std::ranges::contiguous_range<Range> &&
  std::same_as<std::ranges::range_value_t<Range>, Migration> && !std::is_lvalue_reference_v<Range> &&
  !std::ranges::borrowed_range<Range>;

} // namespace detail

/// Convert a migration runner failure to the normal error-code channel.
inline std::error_code make_error_code(MigrationError error) noexcept
{
  static detail::MigrationCategory category;
  return {static_cast<int>(error), category};
}

/// Owned migration description. Default transaction policy requires atomic execution; down is optional and explicit.
struct Migration {
  u64 version = 0;
  std::string name;
  std::vector<schema::Operation> up;
  std::vector<schema::Operation> down;
  MigrationTransaction transaction = MigrationTransaction::required;

  /// Attach explicit rollback operations, in execution order. This does not restore deleted data automatically.
  template <class... Operations>
  Migration rollback(Operations... operations) &&
  {
    down = {std::move(operations)...};
    return std::move(*this);
  }

  /// Explicitly opt out of atomicity; failures leave a durable marker requiring manual recovery.
  Migration nontransactional() &&
  {
    transaction = MigrationTransaction::none;
    return std::move(*this);
  }
};

/// Construct a migration with copied name and owned operations. Validation occurs before database changes.
template <class... Operations>
Migration migration(u64 version, std::string_view name, Operations... operations)
{
  return {.version = version, .name = std::string{name}, .up = {std::move(operations)...}};
}

/// Store differently shaped migration descriptions in a fixed-size array; no caller vector is required.
template <class... Entries>
auto migrations(Entries... entries)
{
  return std::array<Migration, sizeof...(Entries)>{std::move(entries)...};
}

/// Failure phase, including ambiguous commit outcomes; cleanup_error does not replace the primary failure.
enum class MigrationPhase {
  validation,
  lock,
  history,
  apply,
  rollback,
  commit,
  cleanup
};

/// Optional owning diagnostic snapshot. Keep it alive and do not read concurrently while the runner is pending.
struct MigrationFailure {
  u64 version = 0;
  MigrationPhase phase = MigrationPhase::validation;
  std::error_code error;
  // Presence preserves cleanup failure even when an error-code value is zero.
  std::optional<std::error_code> cleanup_error;
  std::string message;
};

/// Owned runner policy; the optional failure destination is borrowed through completion/cancellation cleanup.
/// Empty schema selects the driver's current namespace; empty history_schema follows it. Schemas must already exist.
/// target applies only to forward migration and never requests a downgrade.
/// PostgreSQL lock_timeout accepts zero through 24 hours; zero permits only an immediate lock attempt.
struct MigrationOptions {
  std::optional<u64> target;
  std::string schema;
  std::string history_schema;
  std::string history_table = "weave_migrations";
  std::chrono::milliseconds lock_timeout{30000};
  MigrationFailure *failure = nullptr;
};

/// Explicit rollback request; zero steps is a validated no-op, never an implicit full rollback.
/// Namespace, lock_timeout and borrowed failure destination follow MigrationOptions.
struct RollbackOptions {
  std::size_t steps = 1;
  std::string schema;
  std::string history_schema;
  std::string history_table = "weave_migrations";
  std::chrono::milliseconds lock_timeout{30000};
  MigrationFailure *failure = nullptr;
};

/// Owning summary of completed changes, not a promise that a failed later migration was applied.
struct MigrationReport {
  std::size_t applied = 0;
  std::size_t reverted = 0;
  u64 version = 0;
};

/// Validated manifest entry. applied_at is database timestamp text, empty for pending entries.
struct MigrationStatus {
  u64 version = 0;
  std::string name;
  bool applied = false;
  std::string applied_at;
};

/// Apply pending migrations on an idle, exclusively borrowed connection. The manifest is borrowed from this call
/// through Task completion/cancellation cleanup. Retain it unchanged; rvalue collections are deliberately rejected.
/// Each required migration commits independently. No automatic downgrade, query retry, or schema inference.
template <class Connection>
auto migrate(Connection &connection, std::span<const Migration> manifest, MigrationOptions options = {})
{
  return detail::MigrationDriver<Connection>::migrate(connection, manifest, std::move(options));
}

template <class Connection, std::size_t N>
auto migrate(Connection &connection, const std::array<Migration, N> &manifest, MigrationOptions options = {})
{
  return migrate(connection, std::span<const Migration>{manifest}, std::move(options));
}
template <class Connection, std::size_t N>
auto migrate(Connection &, const std::array<Migration, N> &&, MigrationOptions = {}) = delete;
template <class Connection, detail::TemporaryMigrationCollection Range>
auto migrate(Connection &, Range &&, MigrationOptions = {}) = delete;

/// Inspect history under the migration lock without creating a missing history table. Mismatches and dirty history
/// fail. Uses the same connection/manifest lifetime and exclusive-use requirements as migrate().
template <class Connection>
auto migration_status(Connection &connection, std::span<const Migration> manifest, MigrationOptions options = {})
{
  return detail::MigrationDriver<Connection>::status(connection, manifest, std::move(options));
}

template <class Connection, std::size_t N>
auto migration_status(Connection &connection, const std::array<Migration, N> &manifest, MigrationOptions options = {})
{
  return migration_status(connection, std::span<const Migration>{manifest}, std::move(options));
}
template <class Connection, std::size_t N>
auto migration_status(Connection &, const std::array<Migration, N> &&, MigrationOptions = {}) = delete;
template <class Connection, detail::TemporaryMigrationCollection Range>
auto migration_status(Connection &, Range &&, MigrationOptions = {}) = delete;

/// Execute explicit down operations for the latest applied versions. Never synthesizes reverse operations.
/// Prevalidates every requested rollback; a missing down description prevents all requested changes.
/// Uses the same exclusive connection use and borrowed manifest lifetime as migrate().
template <class Connection>
auto rollback(Connection &connection, std::span<const Migration> manifest, RollbackOptions options = {})
{
  return detail::MigrationDriver<Connection>::rollback(connection, manifest, std::move(options));
}

template <class Connection, std::size_t N>
auto rollback(Connection &connection, const std::array<Migration, N> &manifest, RollbackOptions options = {})
{
  return rollback(connection, std::span<const Migration>{manifest}, std::move(options));
}
template <class Connection, std::size_t N>
auto rollback(Connection &, const std::array<Migration, N> &&, RollbackOptions = {}) = delete;
template <class Connection, detail::TemporaryMigrationCollection Range>
auto rollback(Connection &, Range &&, RollbackOptions = {}) = delete;

} // namespace weave::db

namespace std {

template <>
struct is_error_code_enum<weave::db::MigrationError> : true_type {};

} // namespace std
