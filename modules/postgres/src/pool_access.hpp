#pragma once

#include <weave/postgres/connection.hpp>

namespace weave::pg::detail {

struct PoolAccess {
  static void enroll(Connection &connection) noexcept;
  // All deferred/active tasks and session leases must have ended before return.
  static bool recycle(Connection &connection) noexcept;
};

} // namespace weave::pg::detail
