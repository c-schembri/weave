#pragma once

#include <weave/database/migrations.hpp>
#include <weave/mysql/connection.hpp>
#include <weave/mysql/blocking.hpp>

namespace weave::db::detail {

/// MySQL DDL implicitly commits: all entries must explicitly select MigrationTransaction::none.
/// A session lock serializes runners; interrupted changes leave durable history requiring manual recovery.
template <>
struct MigrationDriver<mysql::Connection> {
  static Task<MigrationReport> migrate(mysql::Connection &, std::span<const Migration>, MigrationOptions);
  static Task<std::vector<MigrationStatus>> status(mysql::Connection &, std::span<const Migration>, MigrationOptions);
  static Task<MigrationReport> rollback(mysql::Connection &, std::span<const Migration>, RollbackOptions);
};

/// Synchronous, caller-thread facade with the same manifest borrowing and recovery contracts.
template <>
struct MigrationDriver<mysql::BlockingConnection> {
  static Result<MigrationReport> migrate(mysql::BlockingConnection &, std::span<const Migration>, MigrationOptions);
  static Result<std::vector<MigrationStatus>> status(
    mysql::BlockingConnection &,
    std::span<const Migration>,
    MigrationOptions);
  static Result<MigrationReport> rollback(mysql::BlockingConnection &, std::span<const Migration>, RollbackOptions);
};

} // namespace weave::db::detail
