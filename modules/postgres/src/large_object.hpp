#pragma once

#include <weave/postgres/large_object.hpp>
#include <weave/io.hpp>

namespace weave::pg::detail {

enum class LargeObjectFunction : std::size_t {
  create,
  open,
  read,
  write,
  seek,
  tell,
  truncate,
  close,
  remove,
  count
};

struct LargeObjectAccess {
  using Borrow = Connection::Borrow;
  static Borrow borrow(Connection &connection) noexcept;
  static Task<u32> resolve(Connection &connection, LargeObjectFunction function);
  static std::size_t chunk_limit(const Connection &connection) noexcept;
  static Context &context(BlockingConnection &connection) noexcept;
  static Connection &session(BlockingConnection &connection) noexcept;
};

} // namespace weave::pg::detail
