#pragma once

#include <weave/postgres/connection.hpp>
#include "row_schema.hpp"

namespace weave::pg::detail {

struct ResultAccess {
  static const std::shared_ptr<const RowSchema> &row_schema(ResultSet &result)
  {
    if (!result.row_schema_)
      result.row_schema_ = make_row_schema(result.columns, schema(result));
    return result.row_schema_;
  }

  static void bind_rows(ResultSet &result)
  {
    if (result.columns.empty())
      return;
    const auto &metadata = row_schema(result);
    for (auto &row : result.rows)
      RowAccess::bind(row, metadata);
  }

  static void share_schema(ResultSet &destination, ResultSet &source)
  {
    destination.row_schema_ = row_schema(source);
  }

  static const ResultArena &schema(ResultSet &result)
  {
    if (!result.schema_storage_.identity()) {
      auto arena = result.columns.get_allocator().arena();
      result.schema_storage_ = arena.identity() ? std::move(arena) : ResultArena::create();
    }
    return result.schema_storage_;
  }

  static ResultArena rows(ResultSet &result)
  {
    auto arena = result.rows.get_allocator().arena();
    if (!arena.identity()) {
      arena = ResultArena::create(result.schema_storage_.heap());
      result.rows = ResultList<Row>{ResultAllocator<Row>{arena}};
    }
    return arena;
  }

  static ResultSet create(ResultHeap heap)
  {
    return ResultSet{heap};
  }
};

} // namespace weave::pg::detail
