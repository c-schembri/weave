#pragma once

#include <weave/mysql/row.hpp>
#include <weave/mysql/statement.hpp>
#include <weave/task.hpp>
#include <weave/tls/context.hpp>
#include <chrono>

namespace weave::mysql {

namespace detail {

struct PoolAccess;
struct MigrationAccess;

} // namespace detail

/// Owned password snapshot. Copies share immutable storage, cleansed when its last owner dies.
/// Best-effort cleansing does not erase caller strings or third-party/allocator history.
class Password {
  struct Storage;
  std::shared_ptr<const Storage> storage_;
  friend class Connection;
  std::string_view view() const noexcept;

public:
  Password() = default;
  /// Copy the borrowed password now, not when a lazy connect Task eventually starts.
  Password(std::string_view password);
  Password(const char *password);
};

/// Owned connect configuration: copy lvalues, transfer with std::move. No ambient files/environment.
/// TLS is required by default. plaintext explicitly disables encryption, not authentication checks.
struct Options {
  std::string host = "127.0.0.1";
  u16 port = 3306;
  std::string user;
  Password password;
  std::string database;
  /// Owned credential policy for private trust/mTLS; must verify both certificate and hostname.
  std::optional<TlsContext> tls;
  /// Explicit weak-mode opt-in. Full cleartext passwords remain forbidden even here.
  bool plaintext = false;
  /// Positive deadline for the entire connect/authentication exchange, not later queries.
  std::chrono::milliseconds connect_timeout{10000};
  /// Maximum logical message size, including all 0xffffff-byte continuation chunks.
  std::size_t max_packet_bytes = 32 * 1024 * 1024;
  /// Delivery budget for wire payload and row/schema sizes; not an exact heap/RSS cap.
  std::size_t max_result_bytes = 64 * 1024 * 1024;
  /// Maximum rows per result, including incremental streaming; not a cap on application-retained Rows.
  std::size_t max_rows = 100000;
  std::size_t max_columns = 1024;
  std::size_t max_results = 256;
  std::size_t max_statements = 256;
  /// Explicit opt-in to semicolon-separated statements; query() must consume every result.
  bool multi_statements = false;
};

/// Owned MySQL server diagnostics; SQLSTATE/message are separate from std::error_code's number.
struct Failure {
  u16 number = 0;
  std::string sqlstate;
  std::string message;
};

/// One owned batch command. prepared also selects binary execution for a zero-parameter statement.
struct Command {
  std::string sql;
  std::vector<Parameter> parameters;
  bool prepared = false;
};

/// Completed command results or a server SQL failure. Transport/protocol/cancellation remain Task errors.
struct Outcome {
  Results results;
  std::optional<Failure> failure;
};

/// Move-only, serialized session on its original transport Context. No concurrent protocol calls.
/// Method Tasks borrow the session before initial suspension; drain them before moving/destruction.
/// Cancellation/transport/protocol/resource failures close the session; completed SQL errors do not.
class Connection {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  struct Borrow {
    Impl *impl = nullptr;
    Borrow() noexcept = default;
    explicit Borrow(Connection &connection) noexcept;
    Borrow(Borrow &&other) noexcept;
    Borrow(const Borrow &) = delete;
    ~Borrow();
    void release() noexcept;
  };

  explicit Connection(std::unique_ptr<Impl> impl) noexcept;
  static Task<Connection> establish(Options options);
  static Task<ResultSet> execute_owned(Borrow borrow, std::string sql, bool migration_owner = false);
  static Task<ResultSet> execute_view(Borrow borrow, std::string_view sql);
  static Task<Results> query_owned(Borrow borrow, std::string sql);
  static Task<Results> query_view(Borrow borrow, std::string_view sql, bool migration_owner = false);
  static Task<Statement> prepare_owned(Borrow borrow, std::string sql);
  static Task<Statement> prepare_view(Borrow borrow, std::string_view sql);
  static Task<RowStream> stream_owned(Borrow borrow, std::string sql);
  static Task<RowStream> stream_view(Borrow borrow, std::string_view sql);
  static Task<ResultSet> bound_owned(Borrow borrow, std::string sql, std::vector<Parameter> parameters);
  static Task<ResultSet> bound_view(Borrow borrow, std::string_view sql, std::span<const ParameterView> parameters);
  static Task<db::detail::ResultList<Outcome>> batch_impl(Borrow borrow, std::vector<Command> commands);
  static Task<void> reset_impl(Borrow borrow);
  static Task<void> finish(Borrow borrow);
  friend Task<Connection> connect(Options options);
  friend class Statement;
  friend class RowStream;
  friend struct detail::PoolAccess;
  friend struct detail::MigrationAccess;

public:
  /// Move an idle session with no deferred/active method Tasks; other movement violates its contract.
  Connection(Connection &&other) noexcept;
  Connection(const Connection &) = delete;
  /// Close an idle session without network protocol shutdown; borrowed Tasks must already be drained.
  ~Connection();

  /// Own SQL before the lazy Task starts (lvalues copy, rvalues move). Return owned single results.
  /// SQL errors propagate as server error_codes; last_failure() retains SQLSTATE and message.
  Task<ResultSet> execute(std::string sql);
  /// Borrow SQL unchanged from this call through completion/cancellation cleanup; results still own data.
  Task<ResultSet> execute_borrowed(std::string_view sql);
  /// Prepare/execute/close one owned parameterized statement; use prepare() for repeated execution.
  Task<ResultSet> execute(std::string sql, std::vector<Parameter> parameters);
  /// Borrow SQL/parameter bytes through one-shot preparation/execution/close cleanup; response owns data.
  Task<ResultSet> execute_borrowed(std::string_view sql, std::span<const ParameterView> parameters);
  /// Own SQL and return every response result. Multi-statement SQL requires explicit Options opt-in.
  Task<Results> query(std::string sql);
  /// Borrow SQL through completion/cancellation cleanup; consume and own all responses.
  Task<Results> query_borrowed(std::string_view sql);
  /// Own/borrow SQL until preparation completes; the returned statement leases this session.
  Task<Statement> prepare(std::string sql);
  /// Borrow SQL through preparation cleanup, not the returned statement's entire lifetime.
  Task<Statement> prepare_borrowed(std::string_view sql);
  /// Own/borrow SQL through start cleanup, then retain only the result/session lease.
  Task<RowStream> stream(std::string sql);
  /// Borrow SQL through startup cleanup, then retain only the stream/session lease.
  Task<RowStream> stream_borrowed(std::string_view sql);
  /// Own all commands, execute sequentially and return ordered SQL outcomes; no wire pipelining or implicit
  /// transaction.
  Task<db::detail::ResultList<Outcome>> batch(std::vector<Command> commands);
  /// Reset server session state with COM_RESET_CONNECTION, restore configured charset/database and invalidate
  /// statements. Requires no outstanding handles/deferred calls; an empty configured database cannot clear a later USE
  /// selection.
  Task<void> reset();
  /// Send COM_QUIT and close; network I/O makes this asynchronous. No server acknowledgement is promised.
  Task<void> disconnect();
  /// Close synchronously when idle. Returns busy during protocol work; deferred Tasks still borrow this session.
  Result<void> close() noexcept;
  /// Local open state, not a network liveness probe. Coordinate inspection with protocol use.
  bool open() const noexcept;
  /// Copy the last completed server diagnostic; returns busy during protocol work, nullopt when absent.
  Result<std::optional<Failure>> last_failure() const;
  /// Copy negotiated TLS metadata; plaintext sessions return nullopt. Requires an idle session.
  Result<std::optional<TlsInfo>> tls_info() const;
};

/// Own all options before suspension and connect on the active Context when run.
/// Timeout includes TCP, greeting, TLS and authentication; cancellation closes/drains setup.
/// Supports caching_sha2_password and mysql_native_password; never sends cleartext passwords without verified TLS.
Task<Connection> connect(Options options);

} // namespace weave::mysql
