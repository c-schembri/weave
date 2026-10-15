#include "migration_compiler.hpp"
#include <weave/database/detail/migration_plan.hpp>
#include <algorithm>
#include <cmath>
#include <charconv>
#include <limits>
#include <openssl/asn1.h>

namespace weave::mysql {

namespace {

namespace schema = db::schema;

std::error_code invalid()
{
  return db::make_error_code(db::MigrationError::invalid_description);
}

class Compiler {
  const db::MigrationOptions &options_;
  std::optional<std::error_code> error_;

  std::string identifier(std::string_view name)
  {
    auto result = detail::migration_identifier(name);
    if (!result) {
      error_ = result.error();
      return {};
    }
    return std::move(*result);
  }

  std::string qualified(const schema::Name &name)
  {
    return identifier(name.schema.empty() ? options_.schema : name.schema) + "." + identifier(name.name);
  }

  std::string names(const std::vector<std::string> &values)
  {
    if (values.empty())
      error_ = invalid();
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
      if (std::find(values.begin(), values.begin() + index, values[index]) != values.begin() + index)
        error_ = invalid();
      if (index)
        result += ", ";
      result += identifier(values[index]);
    }
    return result;
  }

  std::string literal(const schema::Literal &value)
  {
    return std::visit(
      [&]<class T>(const T &input) -> std::string {
        if constexpr (std::same_as<T, std::monostate>)
          return {};
        else if constexpr (std::same_as<T, std::string>)
          return "(" + detail::migration_literal(input) + ")";
        else if constexpr (std::same_as<T, bool>)
          return input ? "1" : "0";
        else {
          if constexpr (std::same_as<T, f64>) {
            if (!std::isfinite(input)) {
              error_ = invalid();
              return {};
            }
          }
          std::array<char, 128> buffer;
          auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), input);
          if (converted.ec != std::errc{}) {
            error_ = invalid();
            return {};
          }
          return {buffer.data(), converted.ptr};
        }
      },
      value);
  }

  bool default_matches(const schema::Column &value)
  {
    if (value.default_literal.index() == 0)
      return true;
    if (value.type == schema::Type::boolean)
      return std::holds_alternative<bool>(value.default_literal);
    if (value.type == schema::Type::binary)
      return false;
    if (value.type == schema::Type::text || value.type == schema::Type::varchar || value.type == schema::Type::date ||
      value.type == schema::Type::timestamp || value.type == schema::Type::decimal)
      return std::holds_alternative<std::string>(value.default_literal) &&
        detail::migration_utf8(std::get<std::string>(value.default_literal));
    if (value.type == schema::Type::float32 || value.type == schema::Type::float64)
      return !std::holds_alternative<bool>(value.default_literal) &&
        !std::holds_alternative<std::string>(value.default_literal);
    i64 limit = std::numeric_limits<i64>::max();
    if (value.type == schema::Type::int16)
      limit = std::numeric_limits<i16>::max();
    if (value.type == schema::Type::int32)
      limit = std::numeric_limits<i32>::max();
    if (auto number = std::get_if<i64>(&value.default_literal))
      return *number >= -limit - 1 && *number <= limit;
    if (auto number = std::get_if<u64>(&value.default_literal))
      return *number <= static_cast<u64>(limit);
    return false;
  }

  std::string column(const schema::Column &value, bool identity_indexed)
  {
    std::string type;
    switch (value.type) {
    case schema::Type::int16:
      type = "SMALLINT";
      break;
    case schema::Type::int32:
      type = "INT";
      break;
    case schema::Type::int64:
      type = "BIGINT";
      break;
    case schema::Type::float32:
      type = "FLOAT";
      break;
    case schema::Type::float64:
      type = "DOUBLE";
      break;
    case schema::Type::boolean:
      type = "BOOLEAN";
      break;
    case schema::Type::text:
      type = "LONGTEXT";
      break;
    case schema::Type::binary:
      type = "LONGBLOB";
      break;
    case schema::Type::date:
      type = "DATE";
      break;
    case schema::Type::timestamp:
      type = "DATETIME(6)";
      break;
    case schema::Type::varchar:
      if (!value.length || value.length > 16383 || value.scale)
        error_ = invalid();
      type = "VARCHAR(" + std::to_string(value.length) + ")";
      break;
    case schema::Type::decimal:
      if (!value.length || value.length > 65 || value.scale > 30 || value.scale > value.length)
        error_ = invalid();
      type = "DECIMAL(" + std::to_string(value.length) + "," + std::to_string(value.scale) + ")";
      break;
    default:
      error_ = db::make_error_code(db::MigrationError::unsupported);
      break;
    }
    if (value.type != schema::Type::varchar && value.type != schema::Type::decimal && (value.length || value.scale))
      error_ = invalid();
    if (value.identity &&
      (value.type != schema::Type::int64 || value.allows_null || value.default_literal.index() ||
        !(identity_indexed || value.primary || value.unique_value)))
      error_ = invalid();
    if ((value.primary && value.allows_null) || !default_matches(value))
      error_ = invalid();

    std::string result = identifier(value.name) + " " + type;
    if (!value.allows_null)
      result += " NOT NULL";
    if (value.identity)
      result += " AUTO_INCREMENT";
    if (value.default_literal.index())
      result += " DEFAULT " + literal(value.default_literal);
    if (value.primary)
      result += " PRIMARY KEY";
    if (value.unique_value)
      result += " UNIQUE";
    if (value.type == schema::Type::boolean)
      result += " CHECK (" + identifier(value.name) + " IN (0, 1))";
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
      auto found = std::find_if(columns.begin(), columns.end(), [&](const auto &column) {
        return column.name == name;
      });
      if (found == columns.end())
        error_ = invalid();
      else if (value.kind == schema::Constraint::Kind::foreign_key && !found->allows_null &&
        (value.delete_action == schema::Action::set_null || value.update_action == schema::Action::set_null))
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
    result += " (" + names(value.columns) + ")";
    if (value.kind == schema::Constraint::Kind::foreign_key) {
      if (value.columns.size() != value.referenced_columns.size())
        error_ = invalid();
      result += " REFERENCES " + qualified(value.referenced_table) + " (" + names(value.referenced_columns) + ")";
      result += " ON DELETE " + action(value.delete_action) + " ON UPDATE " + action(value.update_action);
    } else if (!value.referenced_table.name.empty() || !value.referenced_table.schema.empty() ||
      !value.referenced_columns.empty() || value.delete_action != schema::Action::no_action ||
      value.update_action != schema::Action::no_action)
      error_ = invalid();
    return result;
  }

public:
  explicit Compiler(const db::MigrationOptions &options) : options_(options)
  {
  }

  Result<std::string> compile(const schema::Operation &operation)
  {
    const auto kind = operation.kind;
    if (kind == schema::Operation::Kind::sql) {
      if (operation.vendor != db::Vendor::mysql)
        return std::unexpected(db::make_error_code(db::MigrationError::unsupported));
      if (!db::detail::valid_migration_text(operation.statement) || !detail::migration_utf8(operation.statement) ||
        !operation.object.name.empty() || !operation.object.schema.empty() || !operation.new_name.empty() ||
        !operation.columns.empty() || !operation.constraints.empty() || !operation.index_columns.empty() ||
        operation.unique_index)
        return std::unexpected(invalid());
      return operation.statement;
    }
    const auto &space = operation.object.schema.empty() ? options_.schema : operation.object.schema;
    if (space == options_.history_schema && operation.object.name == options_.history_table)
      return std::unexpected(invalid());
    if (!operation.statement.empty() || (kind != schema::Operation::Kind::create_index && operation.unique_index) ||
      (kind != schema::Operation::Kind::create_table && !operation.constraints.empty()) ||
      (kind != schema::Operation::Kind::create_table && kind != schema::Operation::Kind::add_column &&
        !operation.columns.empty()) ||
      (kind != schema::Operation::Kind::create_index && kind != schema::Operation::Kind::rename_column &&
        !operation.index_columns.empty()))
      return std::unexpected(invalid());
    const bool named = kind == schema::Operation::Kind::rename_table || kind == schema::Operation::Kind::drop_column ||
      kind == schema::Operation::Kind::rename_column || kind == schema::Operation::Kind::create_index ||
      kind == schema::Operation::Kind::drop_index;
    if (!named && !operation.new_name.empty())
      return std::unexpected(invalid());

    const auto object = qualified(operation.object);
    std::string result;
    switch (kind) {
    case schema::Operation::Kind::create_table: {
      if (operation.columns.empty())
        error_ = invalid();
      result = "CREATE TABLE " + object + " (";
      std::size_t primary_keys = 0;
      std::size_t identities = 0;
      for (std::size_t index = 0; index < operation.columns.size(); ++index) {
        const auto &value = operation.columns[index];
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
          if (operation.columns[earlier].name == value.name)
            error_ = invalid();
        }
        primary_keys += value.primary;
        identities += value.identity;
        bool indexed = std::any_of(operation.constraints.begin(), operation.constraints.end(), [&](const auto &item) {
          return (item.kind == schema::Constraint::Kind::primary_key ||
                   item.kind == schema::Constraint::Kind::unique) &&
            !item.columns.empty() && item.columns.front() == value.name;
        });
        if (index)
          result += ", ";
        result += column(value, indexed);
      }
      for (const auto &value : operation.constraints) {
        primary_keys += value.kind == schema::Constraint::Kind::primary_key;
        result += ", " + constraint(value, operation.columns);
      }
      if (primary_keys > 1 || identities > 1)
        error_ = invalid();
      result += ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci";
      break;
    }
    case schema::Operation::Kind::drop_table:
      result = "DROP TABLE " + object;
      break;
    case schema::Operation::Kind::rename_table:
      if (space == options_.history_schema && operation.new_name == options_.history_table)
        error_ = invalid();
      result = "RENAME TABLE " + object + " TO " + qualified(schema::qualified(space, operation.new_name));
      break;
    case schema::Operation::Kind::add_column:
      if (operation.columns.size() != 1)
        error_ = invalid();
      else
        result = "ALTER TABLE " + object + " ADD COLUMN " + column(operation.columns.front(), false);
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
      result += identifier(operation.new_name) + " ON " + object + " (" + names(operation.index_columns) + ")";
      break;
    case schema::Operation::Kind::drop_index:
      if (operation.new_name.empty())
        error_ = db::make_error_code(db::MigrationError::unsupported);
      result = "DROP INDEX " + identifier(operation.new_name) + " ON " + object;
      break;
    default:
      error_ = db::make_error_code(db::MigrationError::unsupported);
      break;
    }
    if (error_)
      return std::unexpected(*error_);
    return result;
  }
};

} // namespace

bool detail::migration_utf8(std::string_view value, bool identifier) noexcept
{
  std::size_t offset = 0;
  while (offset < value.size()) {
    unsigned long codepoint = 0;
    const auto remaining = (std::min)(value.size() - offset, std::size_t{6});
    const auto consumed = UTF8_getc(
      reinterpret_cast<const unsigned char *>(value.data() + offset),
      static_cast<int>(remaining),
      &codepoint);
    const auto limit = identifier ? 0xfffful : 0x10fffful;
    if (consumed <= 0 || codepoint > limit || (codepoint >= 0xd800 && codepoint <= 0xdfff))
      return false;
    offset += static_cast<std::size_t>(consumed);
  }
  return true;
}

Result<std::string> detail::migration_identifier(std::string_view name)
{
  if (!db::detail::valid_migration_text(name) || name.size() > 64 || name.ends_with(' ') || !migration_utf8(name, true))
    return std::unexpected(invalid());
  std::string result = "`";
  for (char value : name) {
    result += value;
    if (value == '`')
      result += '`';
  }
  result += '`';
  return result;
}

std::string detail::migration_literal(std::string_view value)
{
  constexpr std::string_view hex = "0123456789abcdef";
  std::string result = "CONVERT(X'";
  for (unsigned char byte : value) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  result += "' USING utf8mb4)";
  return result;
}

Result<std::string> detail::compile_migration(
  const db::schema::Operation &operation,
  const db::MigrationOptions &options)
{
  return Compiler{options}.compile(operation);
}

} // namespace weave::mysql
