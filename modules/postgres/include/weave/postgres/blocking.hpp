#pragma once

#include <weave/postgres/connection.hpp>

namespace weave::pg {

/// Probe synchronously, copying lvalue options or accepting std::move(options) to avoid that copy.
Result<ServerStatus> ping_blocking(Options options);

/// Caller-thread blocking mixed query/COPY lease; its BlockingConnection must outlive it.
/// Consume next() to completion, then finish() before session reuse. No concurrent method calls.
class BlockingExchange {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit BlockingExchange(std::unique_ptr<Impl> impl) noexcept;
  Impl &state() const noexcept;
  friend class BlockingConnection;

public:
  BlockingExchange(BlockingExchange &&other) noexcept;
  BlockingExchange(const BlockingExchange &) = delete;
  ~BlockingExchange();

  /// Block for an owning wire-order event; nullopt means drained exchange completion.
  Result<std::optional<ExchangeEvent>> next();
  /// Send a COPY payload; borrow bytes only until this call returns.
  Result<void> write(std::span<const std::byte> data);
  /// Close the COPY send half using an owned optional failure message; still drain receive events.
  Result<void> finish_send(std::optional<std::string> error = std::nullopt);
  /// Release a fully drained session lease synchronously; pending exchange state returns busy.
  Result<void> finish();
};

/// Caller-thread pipeline lease with owning enqueue and blocking I/O, not a thread-safe shared session.
/// Its connection must outlive it. Queue explicit Sync barriers, consume all deliveries, then finish().
class BlockingPipeline {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit BlockingPipeline(std::unique_ptr<Impl> impl) noexcept;
  Impl &state() const noexcept;
  friend class BlockingConnection;

public:
  BlockingPipeline(BlockingPipeline &&other) noexcept;
  BlockingPipeline(const BlockingPipeline &) = delete;
  ~BlockingPipeline();

  /// Own/copy a command into the bounded queue and return its ID; no network I/O yet.
  Result<u64> execute(Command command, RowOptions rows = {});
  /// Enqueue an owned prepared-statement invocation; lvalues copy and rvalues transfer.
  Result<u64> execute_prepared(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text,
    RowOptions rows = {});
  /// Enqueue an owned Parse command and return its delivery ID.
  Result<u64> prepare(std::string name, std::string sql, std::vector<u32> types = {});
  /// Enqueue statement metadata inspection using an owned name.
  Result<u64> describe(std::string name);
  /// Enqueue portal metadata inspection using an owned name.
  Result<u64> describe_portal(std::string name);
  /// Enqueue an owned statement close.
  Result<u64> close_prepared(std::string name);
  /// Enqueue an owned portal close.
  Result<u64> close_portal(std::string name);
  /// Enqueue a Sync barrier; recovery happens when the barrier is processed, not at enqueue time.
  Result<u64> sync();
  /// Enqueue protocol Flush, not Sync; does not send or clear SQL abort state.
  Result<void> request_flush();
  /// Block while driving queued sends/receives; use start()/next() for incremental delivery.
  Result<void> flush();
  /// Block until queued requests are sent, without receiving their results.
  Result<void> send();
  /// Block while receiving previously sent work; requires a successful send().
  Result<void> receive();
  /// Schedule combined send/receive on the private driver; next() drives it, not a background thread.
  Result<void> start();
  /// Schedule incremental receive after send(); subsequent next() calls drive progress.
  Result<void> start_receive();
  /// Consume an owning delivery, driving a started operation as needed; nullopt means drained.
  Result<std::optional<PipelineResult>> next();
  /// Release a drained lease; unsent requests, unread results or Sync debt return busy.
  Result<void> finish();
  /// Whether SQL abort recovery is awaiting a processed Sync, not socket health.
  bool aborted() const noexcept;
};

/// Move-only session with a private caller-thread Context; methods block and return Result<T>.
/// Call on its owning thread outside a running Context. Do not concurrently share protocol state.
/// View/span inputs are borrowed for the call only; by-value strings/containers copy lvalues or move rvalues.
class BlockingConnection {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit BlockingConnection(std::unique_ptr<Impl> impl) noexcept;
  Impl &state() const noexcept;
  friend struct detail::LargeObjectAccess;

public:
  /// Establish a synchronous connection with its own configuration.
  /// connect(options) copies; connect(std::move(options)) transfers. The source Options need not outlive the call.
  /// Callbacks are owned; referenced state inside their captures is not lifetime-extended.
  static Result<BlockingConnection> connect(Options options, NoticeHandler notices = {});

  /// Connect with owned options (copy lvalues, transfer rvalues); diagnostic is borrowed only until this call returns.
  static Result<BlockingConnection> connect(Options options, Diagnostic &diagnostic, NoticeHandler notices = {});

  /// Connect with owned options/callbacks; lvalue options copy and std::move(options) transfers.
  /// Callback-captured references remain caller-owned.
  static Result<BlockingConnection> connect(Options options, NoticeHandler notices, Trace trace);

  /// Connect with owned options/callbacks; diagnostic is borrowed only until this call returns.
  /// Lvalue options copy and std::move(options) transfers; callback-captured references remain caller-owned.
  static Result<BlockingConnection> connect(
    Options options,
    Diagnostic &diagnostic,
    NoticeHandler notices,
    Trace trace);

  /// Connect with owned options (copy lvalues, transfer rvalues); report is borrowed only until this call returns.
  static Result<BlockingConnection> connect(Options options, ConnectionReport &report, NoticeHandler notices = {});

  /// Connect with owned options/callbacks; report is borrowed only until this call returns.
  /// Lvalue options copy and std::move(options) transfers; callback-captured references remain caller-owned.
  static Result<BlockingConnection> connect(
    Options options,
    ConnectionReport &report,
    NoticeHandler notices,
    Trace trace);

  BlockingConnection(BlockingConnection &&other) noexcept;
  BlockingConnection(const BlockingConnection &) = delete;
  ~BlockingConnection();

  /// Run owned simple-query SQL; return owning wire-order results or the first SQL/client error.
  Result<Results> query(std::string sql);
  /// Run owned SQL and retain SQL failures as outcomes; client/transport failures still return errors.
  Result<std::vector<Outcome>> query_outcomes(std::string sql);
  /// Begin an owned-SQL mixed query/COPY exchange that leases this session until finish().
  Result<BlockingExchange> exchange(std::string sql, RowOptions rows = {});
  /// Execute owned SQL/parameters using extended protocol; return one owning result.
  Result<ResultSet> execute(std::string sql, std::vector<Parameter> parameters = {}, Format format = Format::text);
  /// Execute owned inputs but retain SQL errors in the Outcome value.
  Result<Outcome> execute_outcome(
    std::string sql,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  /// Create a session-bound prepared statement using owned name, SQL and type IDs.
  Result<void> prepare(std::string name, std::string sql, std::vector<u32> types = {});
  /// Borrow SQL, the parameter array and each parameter's data only until this synchronous call returns.
  /// Keep inputs unchanged during the call; the returned result owns its data.
  Result<ResultSet> execute_borrowed(
    std::string_view sql,
    std::span<const ParameterView> parameters = {},
    Format format = Format::text);
  /// Like execute_borrowed(), including call-scoped input lifetimes, but returns SQL errors as outcomes.
  Result<Outcome> execute_outcome_borrowed(
    std::string_view sql,
    std::span<const ParameterView> parameters = {},
    Format format = Format::text);
  /// Borrow name, SQL and type IDs only until this synchronous call returns; keep their storage unchanged meanwhile.
  Result<void> prepare_borrowed(std::string_view name, std::string_view sql, std::span<const u32> types = {});
  /// Borrow the statement name, parameter array and parameter data only until this call returns.
  /// The result owns its data; the prepared statement itself belongs to this session.
  Result<ResultSet> execute_prepared_borrowed(
    std::string_view name,
    std::span<const ParameterView> parameters = {},
    Format format = Format::text);
  /// Like execute_prepared_borrowed(), including call-scoped input lifetimes, but returns SQL errors as outcomes.
  Result<Outcome> execute_prepared_outcome_borrowed(
    std::string_view name,
    std::span<const ParameterView> parameters = {},
    Format format = Format::text);
  /// Invoke a session-bound statement using owned name/parameters; return an owning result.
  Result<ResultSet> execute_prepared(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  /// Invoke an owned statement name/parameters, retaining SQL failure as an Outcome.
  Result<Outcome> execute_prepared_outcome(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  /// Own all commands and return wire-ordered SQL outcomes; client failures remain errors.
  Result<std::vector<Outcome>> batch(std::vector<Command> commands);
  /// Acquire a pipeline lease synchronously on an idle session; no I/O until driven.
  Result<BlockingPipeline> pipeline();
  /// Own a statement name and retrieve its parameter/column description.
  Result<ResultSet> describe(std::string name);
  /// Own a portal name and retrieve its column description.
  Result<ResultSet> describe_portal(std::string name);
  /// Own a name and close its prepared statement on this session.
  Result<void> close_prepared(std::string name);
  /// Call a PostgreSQL function OID through the fast-path protocol; own parameters and returned value.
  Result<Value> call_function(u32 function, std::vector<Parameter> parameters = {}, Format format = Format::binary);
  /// Own SQL and enter a COPY phase; complete it before ordinary session reuse.
  Result<CopyFormat> start_copy(std::string sql);
  /// Send one CopyData message, borrowing its bytes only until return.
  Result<void> write_copy(std::span<const std::byte> data);
  /// Close only the COPY send half; still drain receive/terminal results.
  Result<void> finish_copy_send();
  /// Receive an owning message; nullopt means EOF, an empty vector means an empty CopyData message.
  Result<std::optional<std::vector<std::byte>>> read_copy();
  /// Read a fragment into nonempty caller storage; end_of_message preserves native COPY boundaries.
  Result<std::optional<CopyRead>> read_copy(std::span<std::byte> buffer);
  /// Finish COPY with an owned optional error, draining terminal results before reuse.
  Result<ResultSet> end_copy(std::optional<std::string> error = std::nullopt);
  /// Copy the retained terminal COPY result when available; no protocol I/O.
  Result<ResultSet> copy_result() const;
  /// Copy all retained terminal COPY results in wire order; no protocol I/O.
  Result<Results> copy_results() const;
  /// Own SQL and start single-row delivery; consume read_row() to completion.
  Result<void> start_rows(std::string sql);
  /// Block for a retained row; nullopt means ReadyForQuery completion, errors remain checked.
  Result<std::optional<Row>> read_row();
  /// Borrow active streaming schema until session/schema replacement or destruction.
  const std::vector<Column> &row_columns() const noexcept;
  /// Bind a session/transaction-scoped portal from owned inputs and return its description.
  Result<ResultSet> open_portal(std::string name, std::string sql, std::vector<Parameter> parameters = {});
  /// Own a portal name and fetch rows; suspended indicates further rows remain.
  Result<ResultSet> fetch(std::string name, u32 rows = 128);
  /// Own a portal name and close it on this session.
  Result<void> close_portal(std::string name);
  /// Block for an owning notification; cannot compete with another protocol reader.
  Result<Notification> wait_notification();
  /// Copy a cancellation snapshot independent of later reset/close, usable through its own driver.
  Result<CancelHandle> cancel_handle() const;
  /// Gracefully send Terminate and close; blocks for protocol I/O.
  Result<void> finish();
  /// Reconnect synchronously using owned options: copy an lvalue or transfer with std::move(options).
  /// The source Options need not outlive this call or the reconnected session.
  Result<void> reset(Options options);

  /// Reset with owned options (copy lvalues, transfer rvalues); diagnostic is borrowed only until this call returns.
  Result<void> reset(Options options, Diagnostic &diagnostic);

  /// Reset with owned options (copy lvalues, transfer rvalues); report is borrowed only until this call returns.
  Result<void> reset(Options options, ConnectionReport &report);
  /// Abort an idle session without a graceful Terminate; borrowed leases can return busy.
  Result<void> close() noexcept;
  /// Mark the session closed and request local transport cancellation, not a server CancelRequest.
  Result<void> cancel() noexcept;
  /// Local session-open state, not a peer liveness probe.
  bool open() const noexcept;
  /// Last tracked transaction state; pending work is in_progress, closed/moved is unknown.
  Transaction transaction() const noexcept;
  /// Pipeline lease/recovery state, not transport health.
  PipelineStatus pipeline_status() const noexcept;
  /// Selected backend PID, excluding its cancellation secret.
  u32 backend_process() const noexcept;
  /// Negotiated PostgreSQL wire version.
  ProtocolVersion protocol_version() const noexcept;
  /// Selected session authentication method.
  Authentication authentication_method() const noexcept;
  /// Whether GSS encrypts this transport, distinct from authentication.
  bool gss_encrypted() const noexcept;
  /// Copy owning negotiated-session metadata without secret credentials.
  Result<ConnectionInfo> info() const;
  /// Copy redacted configuration without ambient source reload.
  Result<OptionsInfo> configuration() const;
  /// Inspect the tracked client encoding without protocol I/O.
  Result<Encoding> client_encoding() const noexcept;
  /// Change client_encoding through protocol I/O and update tracking after success.
  Result<void> set_client_encoding(Encoding encoding);
  /// Borrow password during this call and produce an owned verifier; optional policy may query server settings.
  Result<std::string> password_verifier(std::string user, std::string_view password, PasswordOptions options = {});
  /// Change the named user's password through protocol I/O; caller password is borrowed only for this call.
  Result<ResultSet> change_password(std::string user, std::string_view password, PasswordOptions options = {});
  /// Copy tracked ParameterStatus data; nullopt means unknown, name is borrowed only here.
  Result<std::optional<std::string>> parameter(std::string_view name) const;
  /// Quote a borrowed literal with tracked session encoding and return owning SQL text.
  Result<std::string> escape_literal(std::string_view text) const;
  /// Quote a borrowed identifier with tracked session encoding and return owning SQL text.
  Result<std::string> escape_identifier(std::string_view text) const;
  /// Copy last server diagnostic; non-server failures may have no diagnostic fields.
  Diagnostic last_error() const;
  /// Copy the last error-code/diagnostic pair; absent prior failure yields zero code/empty fields.
  Result<Failure> last_failure() const;
  /// Replace an owned inline noexcept notice observer and return the old observer.
  Result<NoticeHandler> on_notice(NoticeHandler handler);
  /// Replace an owned inline noexcept notification observer and return the old observer.
  Result<NotificationHandler> on_notification(NotificationHandler handler);
  /// Register an owned named lifecycle observer; Event references are invocation-scoped.
  Result<EventId> on_event(std::string name, EventHandler handler);
  /// Attach registered lifecycle observers to an application-created result.
  Result<void> attach_events(ResultSet &result);
  /// Copy shared application data for a registered event ID.
  Result<EventData> event_data(EventId id) const;
  /// Replace event data for a registered ID; unknown IDs return errors.
  Result<void> set_event_data(EventId id, EventData data);
  /// Replace owned tracing policy and return the old policy; application tracing exposes sensitive data.
  Result<Trace> on_trace(Trace trace);
  /// Move all queued notices out and clear the queue, without I/O.
  std::vector<Diagnostic> take_notices();
  /// Move queued notifications out and clear the queue; does not wait for another message.
  std::vector<Notification> take_notifications();
};

} // namespace weave::pg
