#pragma once

#include <weave/task.hpp>
#include <array>
#include <expected>
#include <optional>
#include <string_view>
#include <utility>

namespace weave::db {

/// Named lookup failure: absent column or multiple exact name matches.
enum class ColumnError {
  missing,
  ambiguous
};

/// Synchronously find one exact, case-sensitive name in a driver's indexed metadata range.
/// Borrows columns/name only during this call; does not copy metadata or allocate lookup storage.
template <class Columns>
std::expected<std::size_t, ColumnError> find_column(const Columns &columns, std::string_view name)
{
  std::optional<std::size_t> found;
  for (std::size_t index = 0; index < columns.size(); ++index) {
    if (std::string_view{columns[index].name} != name)
      continue;
    if (found)
      return std::unexpected(ColumnError::ambiguous);
    found = index;
  }
  if (!found)
    return std::unexpected(ColumnError::missing);
  return *found;
}

/// Resolve names once using a Result<size_t>-returning lookup into an owning fixed array.
/// Stops at the first lookup error. Inputs are borrowed only here; caller checks schema before reuse.
template <std::size_t Count, class Lookup>
Result<std::array<std::size_t, Count>> bind_columns(const std::array<std::string_view, Count> &names, Lookup &&lookup)
{
  std::array<std::size_t, Count> indices{};
  for (std::size_t index = 0; index < Count; ++index) {
    auto selected = lookup(names[index]);
    if (!selected)
      return std::unexpected(selected.error());
    indices[index] = *selected;
  }
  return indices;
}

} // namespace weave::db
