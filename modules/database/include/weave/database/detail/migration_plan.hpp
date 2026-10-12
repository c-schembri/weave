#pragma once

#include <weave/database/migrations.hpp>
#include <bit>
#include <charconv>
#include <limits>
#include <algorithm>

namespace weave::db::detail {

inline bool valid_migration_text(std::string_view value, bool allow_empty = false) noexcept
{
  return (allow_empty || !value.empty()) && value.find('\0') == std::string_view::npos;
}

inline Result<void> validate_manifest(std::span<const Migration> manifest)
{
  u64 previous = 0;
  for (const auto &entry : manifest) {
    if (entry.version <= previous || entry.version > static_cast<u64>(std::numeric_limits<i64>::max()) ||
      !valid_migration_text(entry.name) || entry.up.empty() ||
      (entry.transaction != MigrationTransaction::required && entry.transaction != MigrationTransaction::none))
      return std::unexpected(make_error_code(MigrationError::invalid_description));
    previous = entry.version;
  }
  return {};
}

// Length-prefixed, versioned descriptions, independent of generated SQL or struct layout.
class MigrationEncoding {
  std::string bytes_ = "weave-migration-description:1;";

public:
  void text(std::string_view value)
  {
    number(value.size());
    bytes_.append(value);
  }

  template <class T>
  void number(T value)
  {
    std::array<char, 32> buffer;
    auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    bytes_.append(buffer.data(), converted.ptr);
    bytes_ += ':';
  }

  template <class T>
    requires std::is_enum_v<T>
  void tag(T value)
  {
    number(static_cast<unsigned>(value));
  }

  void name(const schema::Name &value)
  {
    text(value.schema);
    text(value.name);
  }

  void names(const std::vector<std::string> &values)
  {
    number(values.size());
    for (const auto &value : values)
      text(value);
  }

  void operations(const std::vector<schema::Operation> &values)
  {
    number(values.size());
    for (const auto &value : values) {
      tag(value.kind);
      name(value.object);
      text(value.new_name);
      number(value.columns.size());
      for (const auto &column : value.columns) {
        text(column.name);
        tag(column.type);
        number(column.length);
        number(column.scale);
        number(static_cast<unsigned>(column.allows_null));
        number(static_cast<unsigned>(column.identity));
        number(static_cast<unsigned>(column.primary));
        number(static_cast<unsigned>(column.unique_value));
        number(column.default_literal.index());
        std::visit(
          [&]<class T>(const T &literal) {
            if constexpr (std::same_as<T, std::string>)
              text(literal);
            else if constexpr (std::same_as<T, f64>)
              number(std::bit_cast<u64>(literal));
            else if constexpr (std::same_as<T, bool>)
              number(static_cast<unsigned>(literal));
            else if constexpr (!std::same_as<T, std::monostate>)
              number(literal);
          },
          column.default_literal);
      }
      number(value.constraints.size());
      for (const auto &constraint : value.constraints) {
        tag(constraint.kind);
        names(constraint.columns);
        name(constraint.referenced_table);
        names(constraint.referenced_columns);
        tag(constraint.delete_action);
        tag(constraint.update_action);
      }
      names(value.index_columns);
      number(static_cast<unsigned>(value.unique_index));
      tag(value.vendor);
      text(value.statement);
    }
  }

  std::string finish() &&
  {
    return std::move(bytes_);
  }
};

inline std::string canonical_migration(const Migration &entry, std::string_view default_schema)
{
  MigrationEncoding encoding;
  encoding.text(default_schema);
  encoding.number(entry.version);
  encoding.text(entry.name);
  encoding.tag(entry.transaction);
  encoding.operations(entry.up);
  encoding.operations(entry.down);
  return std::move(encoding).finish();
}

struct MigrationHistory {
  u64 version = 0;
  std::string name;
  std::string checksum;
  std::string state;
  std::string applied_at;
};

// Applied history must be an exact prefix, including versions not selected by this run's target.
inline Result<void> validate_history(
  std::span<const Migration> manifest,
  std::span<const MigrationHistory> history,
  std::span<const std::string> checksums)
{
  if (history.size() > manifest.size() || checksums.size() != manifest.size())
    return std::unexpected(make_error_code(MigrationError::history_mismatch));
  for (std::size_t index = 0; index < history.size(); ++index) {
    const auto &stored = history[index];
    const auto &expected = manifest[index];
    if (stored.version != expected.version || stored.name != expected.name || stored.checksum != checksums[index])
      return std::unexpected(make_error_code(MigrationError::history_mismatch));
    if (stored.state == "applying" || stored.state == "reverting")
      return std::unexpected(make_error_code(MigrationError::dirty_history));
    if (stored.state != "applied" || stored.applied_at.empty())
      return std::unexpected(make_error_code(MigrationError::history_mismatch));
  }
  return {};
}

inline Result<std::size_t> migration_target(std::span<const Migration> manifest, std::optional<u64> target)
{
  if (!target)
    return manifest.size();
  if (*target == 0)
    return 0;
  for (std::size_t index = 0; index < manifest.size(); ++index) {
    if (manifest[index].version == *target)
      return index + 1;
  }
  return std::unexpected(make_error_code(MigrationError::invalid_target));
}

struct MigrationRange {
  std::size_t first = 0;
  std::size_t last = 0;
};

inline Result<MigrationRange> forward_migrations(
  std::span<const Migration> manifest,
  std::size_t applied,
  std::optional<u64> target)
{
  auto end = migration_target(manifest, target);
  if (!end)
    return std::unexpected(end.error());
  if (*end < applied)
    return std::unexpected(make_error_code(MigrationError::invalid_target));
  return MigrationRange{applied, *end};
}

inline Result<MigrationRange> reverse_migrations(
  std::span<const Migration> manifest,
  std::size_t applied,
  std::size_t steps)
{
  if (applied > manifest.size() || steps > applied)
    return std::unexpected(make_error_code(MigrationError::invalid_target));
  const auto remaining = applied - steps;
  for (std::size_t index = remaining; index < applied; ++index) {
    if (manifest[index].down.empty())
      return std::unexpected(make_error_code(MigrationError::missing_rollback));
  }
  return MigrationRange{remaining, applied};
}

} // namespace weave::db::detail
