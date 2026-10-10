#pragma once

#include <weave/postgres/connection.hpp>

namespace weave::pg::detail {

struct ResultAccess {
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
