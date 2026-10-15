#pragma once

#include <weave/mysql/connection.hpp>

namespace weave::mysql {

class BlockingStatement;
class BlockingRowStream;

/// Caller-thread synchronous facade over the same coroutine engine, with a private Context and no helper thread.
/// Call outside a running Context and do not share concurrently. Child statement/stream wrappers retain the driver.
class BlockingConnection {
  struct Driver;
  std::shared_ptr<Driver> driver_;
  friend class BlockingStatement;
  friend class BlockingRowStream;
  friend struct detail::MigrationAccess;
  explicit BlockingConnection(std::shared_ptr<Driver> driver) noexcept;

public:
  /// Copy/move configuration now and complete connection setup before returning.
  static Result<BlockingConnection> connect(Options options);
  BlockingConnection(BlockingConnection &&) noexcept;
  BlockingConnection(const BlockingConnection &) = delete;
  ~BlockingConnection();
  /// Copy/move SQL into the command and return its single owning text response; no exceptions.
  Result<ResultSet> execute(std::string sql);
  /// Prepare/bind/execute/close a one-shot binary statement, copying lvalues or moving owned inputs.
  Result<ResultSet> execute(std::string sql, std::vector<Parameter> parameters);
  /// Borrow SQL/parameters only until synchronous call return.
  Result<ResultSet> execute_borrowed(std::string_view sql);
  Result<ResultSet> execute_borrowed(std::string_view sql, std::span<const ParameterView> parameters);
  /// Copy/move SQL and consume every result; semicolon SQL needs Options::multi_statements.
  Result<Results> query(std::string sql);
  /// Borrow SQL until call return and consume every result into owning storage.
  Result<Results> query_borrowed(std::string_view sql);
  /// Copy/move SQL for preparation; the returned statement retains this private driver.
  Result<BlockingStatement> prepare(std::string sql);
  /// Borrow SQL only until preparation returns, not for the statement's whole lifetime.
  Result<BlockingStatement> prepare_borrowed(std::string_view sql);
  /// Copy/move SQL for stream startup. An unfinished returned stream exclusively leases the session.
  Result<BlockingRowStream> stream(std::string sql);
  /// Borrow SQL only during startup; returned rows own data and the stream retains the driver.
  Result<BlockingRowStream> stream_borrowed(std::string_view sql);
  /// Copy/move commands and return ordered SQL outcomes; no transaction or wire pipelining.
  Result<db::detail::ResultList<Outcome>> batch(std::vector<Command> commands);
  /// Reset/restore session configuration; requires all outstanding statements/streams/deferred calls gone.
  Result<void> reset();
  /// Send QUIT and TLS close_notify before returning; no server acknowledgement.
  Result<void> disconnect();
  /// Abort an idle session without network shutdown; busy sessions return an error.
  Result<void> close() noexcept;
  /// Local state, not a network liveness check.
  bool open() const noexcept;
  /// Copy the last server SQL diagnostic; nullopt when absent.
  Result<std::optional<Failure>> last_failure() const;
  /// Copy negotiated security metadata; plaintext returns nullopt. Requires an idle session.
  Result<std::optional<TlsInfo>> tls_info() const;
};

/// Blocking statement on a retained private driver. Parameters borrow only for the synchronous call.
class BlockingStatement {
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class BlockingConnection;
  explicit BlockingStatement(std::unique_ptr<Impl> impl) noexcept;

public:
  BlockingStatement(BlockingStatement &&) noexcept;
  BlockingStatement(const BlockingStatement &) = delete;
  ~BlockingStatement();
  /// Copy/move parameters; consume a single binary response, draining/rejecting extras.
  Result<ResultSet> execute(std::vector<Parameter> parameters = {});
  /// Borrow parameter array and values only until call return; result owns data.
  Result<ResultSet> execute_borrowed(std::span<const ParameterView> parameters = {});
  /// Copy/move parameters; consume all binary results in wire order.
  Result<Results> query(std::vector<Parameter> parameters = {});
  /// Borrow parameter array/values until call return; consume all results.
  Result<Results> query_borrowed(std::span<const ParameterView> parameters = {});
  /// Copy/move parameters during startup; return an exclusive session-leasing stream.
  Result<BlockingRowStream> stream(std::vector<Parameter> parameters = {});
  /// Borrow parameters only until startup returns, not while later rows are read.
  Result<BlockingRowStream> stream_borrowed(std::span<const ParameterView> parameters = {});
  /// Borrow one string/blob fragment until transport completion; no server acknowledgement.
  Result<void> send_long_data(u16 parameter, std::span<const std::byte> bytes);
  /// Clear long-data/cursor state and wait for server acknowledgement.
  Result<void> reset();
  /// Send server close and release the statement lease; no reply is expected.
  Result<void> close();
  /// Immutable metadata borrowing this handle's retained state.
  std::span<const Column> parameters() const noexcept;
  std::span<const Column> columns() const noexcept;
};

/// next()/next_result()/finish() drive the caller's event loop; there is no background prefetch.
/// Abandoning an undrained stream retires its session. Returned rows own their data.
class BlockingRowStream {
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class BlockingConnection;
  friend class BlockingStatement;
  explicit BlockingRowStream(std::unique_ptr<Impl> impl) noexcept;

public:
  BlockingRowStream(BlockingRowStream &&) noexcept;
  BlockingRowStream(const BlockingRowStream &) = delete;
  ~BlockingRowStream();
  /// Drive one owning row or current-result EOF; active read failures retire/drain the session.
  Result<std::optional<Row>> next();
  /// After current EOF, advance to another result; false means the whole exchange finished.
  Result<bool> next_result();
  /// Copy current metadata/counts without reading more rows; never returns buffered row data.
  Result<ResultSet> summary() const;
  /// Drain every remaining result before returning; errors/cancellation retire the session.
  Result<void> finish();
  /// Whether the exchange lease was released; this does not imply success after a failure.
  bool done() const noexcept;
};

} // namespace weave::mysql
