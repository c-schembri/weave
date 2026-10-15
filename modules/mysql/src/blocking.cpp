#include <weave/mysql/blocking.hpp>
#include <weave/io.hpp>
#include "migration_access.hpp"

namespace weave::mysql {

struct BlockingConnection::Driver {
  Result<Context> ctx = Context::create();
  std::optional<Connection> connection;
};

struct BlockingStatement::Impl {
  std::shared_ptr<BlockingConnection::Driver> driver;
  Statement statement;

  Impl(std::shared_ptr<BlockingConnection::Driver> owner, Statement handle)
      : driver(std::move(owner)), statement(std::move(handle))
  {
  }
};

struct BlockingRowStream::Impl {
  std::shared_ptr<BlockingConnection::Driver> driver;
  RowStream stream;

  Impl(std::shared_ptr<BlockingConnection::Driver> owner, RowStream rows)
      : driver(std::move(owner)), stream(std::move(rows))
  {
  }
};

BlockingConnection::BlockingConnection(std::shared_ptr<Driver> driver) noexcept : driver_(std::move(driver))
{
}

BlockingConnection::BlockingConnection(BlockingConnection &&) noexcept = default;
BlockingConnection::~BlockingConnection() = default;

Result<BlockingConnection> BlockingConnection::connect(Options options)
{
  auto driver = std::make_shared<Driver>();
  if (!driver->ctx)
    return std::unexpected(driver->ctx.error());
  auto connection = driver->ctx->run(mysql::connect(std::move(options)));
  if (!connection)
    return std::unexpected(connection.error());
  driver->connection.emplace(std::move(*connection));
  return BlockingConnection{std::move(driver)};
}

Result<ResultSet> BlockingConnection::execute(std::string sql)
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->execute(std::move(sql)));
}

Result<ResultSet> BlockingConnection::execute(std::string sql, std::vector<Parameter> parameters)
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->execute(std::move(sql), std::move(parameters)));
}

Result<ResultSet> BlockingConnection::execute_borrowed(std::string_view sql)
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->execute_borrowed(sql));
}

Result<ResultSet> BlockingConnection::execute_borrowed(std::string_view sql, std::span<const ParameterView> parameters)
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->execute_borrowed(sql, parameters));
}

Result<Results> BlockingConnection::query(std::string sql)
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->query(std::move(sql)));
}

Result<Results> BlockingConnection::query_borrowed(std::string_view sql)
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->query_borrowed(sql));
}

Result<BlockingStatement> BlockingConnection::prepare(std::string sql)
{
  weave::detail::require(driver_ != nullptr);
  auto statement = driver_->ctx->run(driver_->connection->prepare(std::move(sql)));
  if (!statement)
    return std::unexpected(statement.error());
  return BlockingStatement{std::make_unique<BlockingStatement::Impl>(driver_, std::move(*statement))};
}

Result<BlockingStatement> BlockingConnection::prepare_borrowed(std::string_view sql)
{
  weave::detail::require(driver_ != nullptr);
  auto statement = driver_->ctx->run(driver_->connection->prepare_borrowed(sql));
  if (!statement)
    return std::unexpected(statement.error());
  return BlockingStatement{std::make_unique<BlockingStatement::Impl>(driver_, std::move(*statement))};
}

Result<BlockingRowStream> BlockingConnection::stream(std::string sql)
{
  weave::detail::require(driver_ != nullptr);
  auto stream = driver_->ctx->run(driver_->connection->stream(std::move(sql)));
  if (!stream)
    return std::unexpected(stream.error());
  return BlockingRowStream{std::make_unique<BlockingRowStream::Impl>(driver_, std::move(*stream))};
}

Result<BlockingRowStream> BlockingConnection::stream_borrowed(std::string_view sql)
{
  weave::detail::require(driver_ != nullptr);
  auto stream = driver_->ctx->run(driver_->connection->stream_borrowed(sql));
  if (!stream)
    return std::unexpected(stream.error());
  return BlockingRowStream{std::make_unique<BlockingRowStream::Impl>(driver_, std::move(*stream))};
}

Result<db::detail::ResultList<Outcome>> BlockingConnection::batch(std::vector<Command> commands)
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->batch(std::move(commands)));
}

Result<void> BlockingConnection::reset()
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->reset());
}

Result<void> BlockingConnection::disconnect()
{
  weave::detail::require(driver_ != nullptr);
  return driver_->ctx->run(driver_->connection->disconnect());
}

Result<void> BlockingConnection::close() noexcept
{
  weave::detail::require(driver_ != nullptr);
  return driver_->connection->close();
}

bool BlockingConnection::open() const noexcept
{
  return driver_ && driver_->connection->open();
}

Result<std::optional<Failure>> BlockingConnection::last_failure() const
{
  weave::detail::require(driver_ != nullptr);
  return driver_->connection->last_failure();
}

BlockingStatement::BlockingStatement(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

BlockingStatement::BlockingStatement(BlockingStatement &&) noexcept = default;
BlockingStatement::~BlockingStatement() = default;

Result<ResultSet> BlockingStatement::execute(std::vector<Parameter> parameters)
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->statement.execute(std::move(parameters)));
}

Result<ResultSet> BlockingStatement::execute_borrowed(std::span<const ParameterView> parameters)
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->statement.execute_borrowed(parameters));
}

Result<Results> BlockingStatement::query(std::vector<Parameter> parameters)
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->statement.query(std::move(parameters)));
}

Result<Results> BlockingStatement::query_borrowed(std::span<const ParameterView> parameters)
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->statement.query_borrowed(parameters));
}

Result<BlockingRowStream> BlockingStatement::stream(std::vector<Parameter> parameters)
{
  weave::detail::require(impl_ != nullptr);
  auto stream = impl_->driver->ctx->run(impl_->statement.stream(std::move(parameters)));
  if (!stream)
    return std::unexpected(stream.error());
  return BlockingRowStream{std::make_unique<BlockingRowStream::Impl>(impl_->driver, std::move(*stream))};
}

Result<BlockingRowStream> BlockingStatement::stream_borrowed(std::span<const ParameterView> parameters)
{
  weave::detail::require(impl_ != nullptr);
  auto stream = impl_->driver->ctx->run(impl_->statement.stream_borrowed(parameters));
  if (!stream)
    return std::unexpected(stream.error());
  return BlockingRowStream{std::make_unique<BlockingRowStream::Impl>(impl_->driver, std::move(*stream))};
}

Result<void> BlockingStatement::send_long_data(u16 parameter, std::span<const std::byte> bytes)
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->statement.send_long_data(parameter, bytes));
}

Result<void> BlockingStatement::reset()
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->statement.reset());
}

Result<void> BlockingStatement::close()
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->statement.close());
}

std::span<const Column> BlockingStatement::parameters() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return impl_->statement.parameters();
}

std::span<const Column> BlockingStatement::columns() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return impl_->statement.columns();
}

BlockingRowStream::BlockingRowStream(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

BlockingRowStream::BlockingRowStream(BlockingRowStream &&) noexcept = default;
BlockingRowStream::~BlockingRowStream() = default;

Result<std::optional<Row>> BlockingRowStream::next()
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->stream.next());
}

Result<bool> BlockingRowStream::next_result()
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->stream.next_result());
}

Result<ResultSet> BlockingRowStream::summary() const
{
  weave::detail::require(impl_ != nullptr);
  return impl_->stream.summary();
}

Result<void> BlockingRowStream::finish()
{
  weave::detail::require(impl_ != nullptr);
  return impl_->driver->ctx->run(impl_->stream.finish());
}

bool BlockingRowStream::done() const noexcept
{
  return impl_ && impl_->stream.done();
}

Result<std::optional<TlsInfo>> BlockingConnection::tls_info() const
{
  weave::detail::require(driver_ != nullptr);
  return driver_->connection->tls_info();
}

Context &detail::MigrationAccess::context(BlockingConnection &connection) noexcept
{
  weave::detail::require(connection.driver_ != nullptr);
  return *connection.driver_->ctx;
}

Connection &detail::MigrationAccess::session(BlockingConnection &connection) noexcept
{
  weave::detail::require(connection.driver_ != nullptr);
  return *connection.driver_->connection;
}

} // namespace weave::mysql
