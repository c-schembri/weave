#pragma once

#include <weave/postgres/connection.hpp>

namespace weave::pg {

Result<ServerStatus> ping_blocking(Options options);

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

  Result<std::optional<ExchangeEvent>> next();
  Result<void> write(std::span<const std::byte> data);
  Result<void> finish_send(std::optional<std::string> error = std::nullopt);
  Result<void> finish();
};

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

  Result<u64> execute(Command command, RowOptions rows = {});
  Result<u64> execute_prepared(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text,
    RowOptions rows = {});
  Result<u64> prepare(std::string name, std::string sql, std::vector<u32> types = {});
  Result<u64> describe(std::string name);
  Result<u64> describe_portal(std::string name);
  Result<u64> close_prepared(std::string name);
  Result<u64> close_portal(std::string name);
  Result<u64> sync();
  Result<void> request_flush();
  Result<void> flush();
  Result<void> send();
  Result<void> receive();
  Result<void> start();
  Result<void> start_receive();
  Result<std::optional<PipelineResult>> next();
  Result<void> finish();
  bool aborted() const noexcept;
};

class BlockingConnection {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit BlockingConnection(std::unique_ptr<Impl> impl) noexcept;
  Impl &state() const noexcept;
  friend struct detail::LargeObjectAccess;

public:
  static Result<BlockingConnection> connect(Options options, NoticeHandler notices = {});
  static Result<BlockingConnection> connect(Options options, Diagnostic &diagnostic, NoticeHandler notices = {});
  static Result<BlockingConnection> connect(Options options, NoticeHandler notices, Trace trace);
  static Result<BlockingConnection> connect(
    Options options,
    Diagnostic &diagnostic,
    NoticeHandler notices,
    Trace trace);
  static Result<BlockingConnection> connect(Options options, ConnectionReport &report, NoticeHandler notices = {});
  static Result<BlockingConnection> connect(
    Options options,
    ConnectionReport &report,
    NoticeHandler notices,
    Trace trace);

  BlockingConnection(BlockingConnection &&other) noexcept;
  BlockingConnection(const BlockingConnection &) = delete;
  ~BlockingConnection();

  Result<Results> query(std::string sql);
  Result<std::vector<Outcome>> query_outcomes(std::string sql);
  Result<BlockingExchange> exchange(std::string sql, RowOptions rows = {});
  Result<ResultSet> execute(std::string sql, std::vector<Parameter> parameters = {}, Format format = Format::text);
  Result<Outcome> execute_outcome(
    std::string sql,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  Result<void> prepare(std::string name, std::string sql, std::vector<u32> types = {});
  Result<ResultSet> execute_prepared(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  Result<Outcome> execute_prepared_outcome(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  Result<std::vector<Outcome>> batch(std::vector<Command> commands);
  Result<BlockingPipeline> pipeline();
  Result<ResultSet> describe(std::string name);
  Result<ResultSet> describe_portal(std::string name);
  Result<void> close_prepared(std::string name);
  Result<Value> call_function(u32 function, std::vector<Parameter> parameters = {}, Format format = Format::binary);
  Result<CopyFormat> start_copy(std::string sql);
  Result<void> write_copy(std::span<const std::byte> data);
  Result<void> finish_copy_send();
  Result<std::optional<std::vector<std::byte>>> read_copy();
  Result<ResultSet> end_copy(std::optional<std::string> error = std::nullopt);
  Result<ResultSet> copy_result() const;
  Result<Results> copy_results() const;
  Result<void> start_rows(std::string sql);
  Result<std::optional<Row>> read_row();
  const std::vector<Column> &row_columns() const noexcept;
  Result<ResultSet> open_portal(std::string name, std::string sql, std::vector<Parameter> parameters = {});
  Result<ResultSet> fetch(std::string name, u32 rows = 128);
  Result<void> close_portal(std::string name);
  Result<Notification> wait_notification();
  Result<CancelHandle> cancel_handle() const;
  Result<void> finish();
  Result<void> reset(Options options);
  Result<void> reset(Options options, Diagnostic &diagnostic);
  Result<void> reset(Options options, ConnectionReport &report);
  Result<void> close() noexcept;
  Result<void> cancel() noexcept;
  bool open() const noexcept;
  Transaction transaction() const noexcept;
  PipelineStatus pipeline_status() const noexcept;
  u32 backend_process() const noexcept;
  ProtocolVersion protocol_version() const noexcept;
  Authentication authentication_method() const noexcept;
  bool gss_encrypted() const noexcept;
  Result<ConnectionInfo> info() const;
  Result<OptionsInfo> configuration() const;
  Result<Encoding> client_encoding() const noexcept;
  Result<void> set_client_encoding(Encoding encoding);
  Result<std::string> password_verifier(std::string user, std::string_view password, PasswordOptions options = {});
  Result<ResultSet> change_password(std::string user, std::string_view password, PasswordOptions options = {});
  Result<std::optional<std::string>> parameter(std::string_view name) const;
  Result<std::string> escape_literal(std::string_view text) const;
  Result<std::string> escape_identifier(std::string_view text) const;
  Diagnostic last_error() const;
  Result<Failure> last_failure() const;
  Result<NoticeHandler> on_notice(NoticeHandler handler);
  Result<NotificationHandler> on_notification(NotificationHandler handler);
  Result<EventId> on_event(std::string name, EventHandler handler);
  Result<void> attach_events(ResultSet &result);
  Result<EventData> event_data(EventId id) const;
  Result<void> set_event_data(EventId id, EventData data);
  Result<Trace> on_trace(Trace trace);
  std::vector<Diagnostic> take_notices();
  std::vector<Notification> take_notifications();
};

} // namespace weave::pg
