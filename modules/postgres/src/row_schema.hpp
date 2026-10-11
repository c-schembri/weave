#pragma once

#include <weave/postgres/row.hpp>

namespace weave::pg::detail {

struct RowSchema {
  ResultList<Column> columns;

  explicit RowSchema(std::span<const Column> source, ResultArena arena = {}) : columns(ResultAllocator<Column>{arena})
  {
    columns.reserve(source.size());
    for (const auto &column : source) {
      columns.push_back(
        {ResultText{column.name.data(), column.name.size(), ResultAllocator<char>{arena}},
          column.table,
          column.attribute,
          column.type,
          column.type_size,
          column.modifier,
          column.format});
    }
  }
};

inline std::shared_ptr<const RowSchema> make_row_schema(std::span<const Column> columns, ResultArena arena = {})
{
  if (!arena.identity())
    arena = ResultArena::create();
  return std::allocate_shared<RowSchema>(ResultAllocator<RowSchema>{arena}, columns, arena);
}

struct RowAccess {
  static void bind(Row &row, std::shared_ptr<const RowSchema> schema)
  {
    row.schema_ = std::move(schema);
  }

  static const std::shared_ptr<const RowSchema> &schema(const Row &row)
  {
    return row.schema_;
  }
};

} // namespace weave::pg::detail
