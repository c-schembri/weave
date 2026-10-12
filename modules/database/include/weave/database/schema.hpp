#pragma once

#include <weave/types.hpp>
#include <concepts>
#include <string>
#include <string_view>
#include <vector>
#include <variant>
#include <utility>
#include <type_traits>

namespace weave::db {

/// Explicit SQL dialect for an escape hatch; never translated into another vendor's SQL.
enum class Vendor {
  postgres,
  mysql,
  sqlite,
  oracle,
  mssql
};

namespace schema {

/// Logical schema types, compiled by the selected driver rather than inferred from C++ records.
enum class Type {
  int16,
  int32,
  int64,
  float32,
  float64,
  boolean,
  text,
  varchar,
  binary,
  date,
  timestamp,
  decimal
};
/// Referential actions supported by the schema vocabulary; a driver may reject unsupported combinations.
enum class Action {
  no_action,
  restrict,
  cascade,
  set_null
};
/// Owning literal default. Strings are values, never SQL expressions; monostate means no default.
using Literal = std::variant<std::monostate, bool, i64, u64, f64, std::string>;

/// An identifier with optional explicit schema. Dots in name are literal, not qualification syntax.
struct Name {
  std::string name;
  std::string schema;

  Name() = default;

  Name(const char *value) : name(value)
  {
  }

  Name(std::string_view value) : name(value)
  {
  }

  Name(std::string value) : name(std::move(value))
  {
  }
};

/// Build an explicitly schema-qualified name; both inputs are copied.
inline Name qualified(std::string_view space, std::string_view name)
{
  Name result{name};
  result.schema = space;
  return result;
}

/// Owning column description. Fluent modifiers return a value, safe for temporary builder expressions.
struct Column {
  std::string name;
  Type type = Type::text;
  u32 length = 0;
  u32 scale = 0;
  bool allows_null = true;
  bool identity = false;
  bool primary = false;
  bool unique_value = false;
  Literal default_literal;

  /// Require a non-NULL value.
  Column not_null() &&
  {
    allows_null = false;
    return std::move(*this);
  }

  /// Permit NULL values; primary keys remain non-NULL at the database boundary.
  Column nullable() &&
  {
    allows_null = true;
    return std::move(*this);
  }

  /// Make this column the table's single-column primary key.
  Column primary_key() &&
  {
    primary = true;
    allows_null = false;
    return std::move(*this);
  }

  /// Add a single-column uniqueness constraint; NULL semantics remain vendor-specific.
  Column unique() &&
  {
    unique_value = true;
    return std::move(*this);
  }

  /// Set a copied string default, quoted by the dialect compiler.
  Column default_value(std::string_view value) &&
  {
    default_literal = std::string{value};
    return std::move(*this);
  }

  /// Set a boolean default; not an integer conversion.
  Column default_value(bool value) &&
  {
    default_literal = value;
    return std::move(*this);
  }

  /// Set an exact signed or unsigned integer default.
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  Column default_value(T value) &&
  {
    if constexpr (std::is_signed_v<T>)
      default_literal = static_cast<i64>(value);
    else
      default_literal = static_cast<u64>(value);
    return std::move(*this);
  }

  /// Set a finite floating-point default; invalid values are rejected during validation.
  template <std::floating_point T>
  Column default_value(T value) &&
  {
    default_literal = static_cast<f64>(value);
    return std::move(*this);
  }

  /// String literals are strings, not pointer-to-bool conversions.
  Column default_value(const char *value) &&
  {
    return std::move(*this).default_value(std::string_view{value});
  }
};

/// Define a column with an explicit logical type; its name is copied.
inline Column column(std::string_view name, Type type)
{
  return Column{.name = std::string{name}, .type = type};
}

/// Database-generated signed 64-bit identity. Sequence allocation semantics remain vendor-specific.
inline Column identity(std::string_view name)
{
  auto result = column(name, Type::int64);
  result.identity = true;
  result.allows_null = false;
  return result;
}

/// Explicit signed integer columns; no unsigned-storage approximation.
inline Column int16(std::string_view name)
{
  return column(name, Type::int16);
}

inline Column int32(std::string_view name)
{
  return column(name, Type::int32);
}

inline Column int64(std::string_view name)
{
  return column(name, Type::int64);
}

/// Explicit floating-point columns; no fixed-point precision promise.
inline Column float32(std::string_view name)
{
  return column(name, Type::float32);
}

inline Column float64(std::string_view name)
{
  return column(name, Type::float64);
}

/// Boolean and variable-length text columns; text comparison semantics remain vendor-specific.
inline Column boolean(std::string_view name)
{
  return column(name, Type::boolean);
}

inline Column text(std::string_view name)
{
  return column(name, Type::text);
}

/// Bounded character column; the driver validates the positive length.
inline Column varchar(std::string_view name, u32 length)
{
  auto result = column(name, Type::varchar);
  result.length = length;
  return result;
}

/// Variable-length binary data, not text with an encoding.
inline Column binary(std::string_view name)
{
  return column(name, Type::binary);
}

/// Calendar date and timestamp without time zone; no implicit UTC conversion.
inline Column date(std::string_view name)
{
  return column(name, Type::date);
}

inline Column timestamp(std::string_view name)
{
  return column(name, Type::timestamp);
}

/// Exact decimal precision/scale; unsupported ranges fail rather than selecting a floating-point type.
inline Column decimal(std::string_view name, u32 precision, u32 scale)
{
  auto result = column(name, Type::decimal);
  result.length = precision;
  result.scale = scale;
  return result;
}

/// Owning table constraint. Column order matters for composite keys and references.
struct Constraint {
  enum class Kind {
    primary_key,
    unique,
    foreign_key
  };
  Kind kind = Kind::unique;
  std::vector<std::string> columns;
  Name referenced_table;
  std::vector<std::string> referenced_columns;
  Action delete_action = Action::no_action;
  Action update_action = Action::no_action;

  /// Set the referenced table and ordered columns, copying their names.
  template <class... Names>
  Constraint references(Name table, Names... names) &&
  {
    referenced_table = std::move(table);
    referenced_columns = {std::string{names}...};
    return std::move(*this);
  }

  /// Select the explicit delete/update action; no implicit cascade.
  Constraint on_delete(Action action) &&
  {
    delete_action = action;
    return std::move(*this);
  }

  Constraint on_update(Action action) &&
  {
    update_action = action;
    return std::move(*this);
  }
};

/// Define an ordered composite primary key.
template <class... Names>
Constraint primary_key(Names... names)
{
  return {.kind = Constraint::Kind::primary_key, .columns = {std::string{names}...}};
}

/// Define an ordered uniqueness constraint; NULL behavior remains vendor-specific.
template <class... Names>
Constraint unique(Names... names)
{
  return {.kind = Constraint::Kind::unique, .columns = {std::string{names}...}};
}

/// Define referencing columns; complete the description with references().
template <class... Names>
Constraint foreign_key(Names... names)
{
  return {.kind = Constraint::Kind::foreign_key, .columns = {std::string{names}...}};
}

/// Owning schema operation. Description construction does not execute or validate database SQL.
struct Operation {
  enum class Kind {
    create_table,
    drop_table,
    rename_table,
    add_column,
    drop_column,
    rename_column,
    create_index,
    drop_index,
    sql
  };
  Kind kind = Kind::create_table;
  Name object;
  std::string new_name;
  std::vector<Column> columns;
  std::vector<Constraint> constraints;
  std::vector<std::string> index_columns;
  bool unique_index = false;
  Vendor vendor = Vendor::postgres;
  std::string statement;

  /// Make a create_index operation unique.
  Operation unique() &&
  {
    unique_index = true;
    return std::move(*this);
  }
};

/// Describe a table from columns and table constraints; inputs are copied or moved into owned storage.
template <class... Elements>
Operation create_table(Name name, Elements... elements)
{
  Operation operation{.object = std::move(name)};
  auto add = [&]<class T>(T &element) {
    if constexpr (std::same_as<T, Column>)
      operation.columns.push_back(std::move(element));
    else {
      static_assert(std::same_as<T, Constraint>);
      operation.constraints.push_back(std::move(element));
    }
  };
  (add(elements), ...);
  return operation;
}

/// Drop a table without implicit CASCADE or IF EXISTS.
inline Operation drop_table(Name name)
{
  return {.kind = Operation::Kind::drop_table, .object = std::move(name)};
}

/// Rename a table within its existing schema.
inline Operation rename_table(Name name, std::string_view replacement)
{
  return {.kind = Operation::Kind::rename_table, .object = std::move(name), .new_name = std::string{replacement}};
}

/// Add one explicitly described column.
inline Operation add_column(Name table, Column column)
{
  return {.kind = Operation::Kind::add_column, .object = std::move(table), .columns = {std::move(column)}};
}

/// Drop a column without implicit CASCADE.
inline Operation drop_column(Name table, std::string_view name)
{
  return {.kind = Operation::Kind::drop_column, .object = std::move(table), .new_name = std::string{name}};
}

/// Rename a column without changing its type or constraints.
inline Operation rename_column(Name table, std::string_view name, std::string_view replacement)
{
  return {
    .kind = Operation::Kind::rename_column,
    .object = std::move(table),
    .new_name = std::string{replacement},
    .index_columns = {std::string{name}}};
}

/// Create an ordered, plain-column index in its table's schema. Expressions require vendor SQL.
template <class... Names>
Operation create_index(std::string_view name, Name table, Names... columns)
{
  return {
    .kind = Operation::Kind::create_index,
    .object = std::move(table),
    .new_name = std::string{name},
    .index_columns = {std::string{columns}...}};
}

/// Drop an explicitly named index without implicit CASCADE.
inline Operation drop_index(Name name)
{
  return {.kind = Operation::Kind::drop_index, .object = std::move(name)};
}

} // namespace schema

/// Own one trusted, vendor-specific SQL statement. Never include transaction control or change migration locks/history.
/// The driver does not rewrite SQL, infer reversibility, or validate arbitrary SQL's effects in advance.
inline schema::Operation sql(Vendor vendor, std::string_view statement)
{
  return {.kind = schema::Operation::Kind::sql, .vendor = vendor, .statement = std::string{statement}};
}

} // namespace weave::db
