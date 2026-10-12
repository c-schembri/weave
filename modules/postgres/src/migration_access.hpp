#pragma once

#include <weave/postgres/connection.hpp>

namespace weave::pg::detail {

// Retain the existing deferred-Task lifetime lease without exposing it in the public migration API.
struct MigrationAccess {
  using Borrow = Connection::Borrow;
  static Borrow borrow(Connection &connection) noexcept;
  static Result<u32> server_version(Connection &connection) noexcept;
  static Result<Failure> failure(Connection &connection);
};

} // namespace weave::pg::detail
