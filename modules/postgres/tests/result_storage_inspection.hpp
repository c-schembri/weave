#pragma once

#include <weave/postgres.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>
#include <vector>

namespace memory_test {

namespace pg = weave::pg;
namespace storage = weave::pg::detail;

inline std::atomic<unsigned> checks{0};
inline std::atomic<unsigned> inspections{0};

inline void check(bool condition, std::source_location location = std::source_location::current())
{
  checks.fetch_add(1, std::memory_order_relaxed);
  if (!condition) {
    std::fprintf(stderr, "Memory path check failed: %s:%u\n", location.file_name(), location.line());
    std::fflush(stderr);
    std::abort();
  }
}

inline void inspect(const pg::ResultSet &original)
{
  inspections.fetch_add(1, std::memory_order_relaxed);
  const auto before = original.memory_size();
  auto copy = original.copy({.observers = false});
  std::vector<const void *> seen;
  std::size_t bytes = 0;
  auto count = [&](const auto &container) {
    const auto arena = container.get_allocator().arena();
    if (arena.identity() && std::find(seen.begin(), seen.end(), arena.identity()) == seen.end()) {
      seen.push_back(arena.identity());
      bytes += arena.bytes();
    }
  };
  count(copy.columns);
  count(copy.rows);
  count(copy.command);
  count(copy.parameter_types);
  for (const auto &column : copy.columns)
    count(column.name);
  for (const auto &row : copy.rows) {
    count(row);
    for (const auto &value : row) {
      if (value.data)
        count(*value.data);
    }
  }
  check(copy.memory_size() == bytes);
  check(original.memory_size() == before);
  check(copy.kind == original.kind && copy.suspended == original.suspended);
  check(copy.command == original.command);
  check(copy.parameter_types == original.parameter_types);
  check(copy.columns.size() == original.columns.size());
  check(copy.rows.size() == original.rows.size());
  for (std::size_t index = 0; index < copy.columns.size(); ++index) {
    const auto &left = copy.columns[index];
    const auto &right = original.columns[index];
    check(left.name == right.name && left.type == right.type && left.format == right.format);
    check(left.table == right.table && left.attribute == right.attribute);
    check(left.type_size == right.type_size && left.modifier == right.modifier);
  }
  for (std::size_t index = 0; index < copy.rows.size(); ++index) {
    const auto &left = copy.rows[index];
    const auto &right = original.rows[index];
    check(left.size() == right.size());
    for (std::size_t field = 0; field < left.size(); ++field) {
      check(left[field].format == right[field].format);
      check(left[field].is_null() == right[field].is_null());
      check(left[field].bytes() == right[field].bytes());
    }
  }
}

inline void own_row(const pg::Row &row)
{
  const auto arena = row.get_allocator().arena();
  check(arena.identity() != nullptr && arena.bytes() != 0);
  for (const auto &value : row) {
    if (value.data)
      check(value.data->get_allocator().arena().identity() == arena.identity());
  }
}

} // namespace memory_test
