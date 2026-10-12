#pragma once

#include <weave/database/migrations.hpp>

namespace weave::db::detail {

inline Migration manifest_entry(Migration entry, u64 version, std::string_view name)
{
  // A mismatched function is a runtime description error, never a fatal assertion or silently rewritten history.
  if (entry.version != version || entry.name != name)
    entry.version = 0;
  return entry;
}

} // namespace weave::db::detail
