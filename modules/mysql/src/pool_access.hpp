#pragma once

#include <weave/mysql/connection.hpp>

namespace weave::mysql::detail {

// The pool needs session lifetime validation, not access to vendor transport/protocol state.
struct PoolAccess {
  static void enroll(Connection &connection) noexcept;
  static bool recycle(Connection &connection) noexcept;
};

} // namespace weave::mysql::detail
