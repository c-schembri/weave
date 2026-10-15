#pragma once

#include <weave/mysql/connection.hpp>
#include <weave/mysql/blocking.hpp>

namespace weave {

class Context;

}

namespace weave::mysql::detail {

struct MigrationAccess {
  struct Lease {
    Connection *connection;
    Connection::Borrow borrow;
    bool acquired = false;

    explicit Lease(Connection &session) noexcept;
    Lease(Lease &&other) noexcept;
    Lease(const Lease &) = delete;
    ~Lease();
    Result<void> acquire() noexcept;
  };

  static Lease borrow(Connection &connection) noexcept;
  static Task<ResultSet> execute(Lease &lease, std::string sql);
  static bool idle(const Lease &lease) noexcept;
  static void close(Lease &lease) noexcept;
  static Context &context(BlockingConnection &connection) noexcept;
  static Connection &session(BlockingConnection &connection) noexcept;
};

} // namespace weave::mysql::detail
