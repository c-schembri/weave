#include <weave/postgres/blocking.hpp>
#include <weave/io.hpp>
#include "large_object.hpp"
#include "options.hpp"

namespace weave::pg {

namespace {

Task<void> join_pipeline(JoinHandle<void> job)
{
  co_await std::move(job);
}

} // namespace

Result<ServerStatus> ping_blocking(Options options)
{
  detail::OptionsCleanup cleanup{options};
  auto ctx = Context::create();
  if (!ctx)
    return std::unexpected(ctx.error());
  return ctx->run(pg::ping(std::move(options)));
}

struct BlockingExchange::Impl {
  Context *ctx;
  Exchange exchange;

  Impl(Context &owner, Exchange session) : ctx(&owner), exchange(std::move(session))
  {
  }
};

BlockingExchange::BlockingExchange(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

BlockingExchange::BlockingExchange(BlockingExchange &&other) noexcept = default;
BlockingExchange::~BlockingExchange() = default;

BlockingExchange::Impl &BlockingExchange::state() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return *impl_;
}

Result<std::optional<ExchangeEvent>> BlockingExchange::next()
{
  if (!state().ctx)
    return std::optional<ExchangeEvent>{};
  return state().ctx->run(state().exchange.next());
}

Result<void> BlockingExchange::write(std::span<const std::byte> data)
{
  if (!state().ctx)
    return std::unexpected(make_error_code(Error::closed));
  return state().ctx->run(state().exchange.write(data));
}

Result<void> BlockingExchange::finish_send(std::optional<std::string> error)
{
  if (!state().ctx)
    return std::unexpected(make_error_code(Error::closed));
  return state().ctx->run(state().exchange.finish_send(std::move(error)));
}

Result<void> BlockingExchange::finish()
{
  auto status = state().exchange.finish();
  if (status)
    state().ctx = nullptr;
  return status;
}

struct BlockingPipeline::Impl {
  Context *ctx;
  Pipeline pipeline;
  std::optional<JoinHandle<void>> driver;
  bool streaming = false;
  bool sent = false;

  Impl(Context &owner, Pipeline session) : ctx(&owner), pipeline(std::move(session))
  {
  }

  ~Impl()
  {
    if (driver) {
      driver->cancel();
      static_cast<void>(ctx->run(join_pipeline(std::move(*driver))));
    }
  }

  Result<void> queueable() const
  {
    if (driver || sent)
      return std::unexpected(make_error_code(Error::busy));
    return {};
  }
};

BlockingPipeline::BlockingPipeline(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

BlockingPipeline::BlockingPipeline(BlockingPipeline &&other) noexcept = default;
BlockingPipeline::~BlockingPipeline() = default;

BlockingPipeline::Impl &BlockingPipeline::state() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return *impl_;
}

Result<u64> BlockingPipeline::execute(Command command, RowOptions rows)
{
  auto &impl = state();
  if (auto status = impl.queueable(); !status)
    return std::unexpected(status.error());

  auto result = impl.pipeline.execute(std::move(command), rows);
  if (result && rows.chunk_rows)
    impl.streaming = true;
  return result;
}

Result<u64> BlockingPipeline::execute_prepared(
  std::string name,
  std::vector<Parameter> parameters,
  Format format,
  RowOptions rows)
{
  auto &impl = state();
  if (auto status = impl.queueable(); !status)
    return std::unexpected(status.error());

  auto result = impl.pipeline.execute_prepared(std::move(name), std::move(parameters), format, rows);
  if (result && rows.chunk_rows)
    impl.streaming = true;
  return result;
}

Result<u64> BlockingPipeline::prepare(std::string name, std::string sql, std::vector<u32> types)
{
  if (auto status = state().queueable(); !status)
    return std::unexpected(status.error());
  return state().pipeline.prepare(std::move(name), std::move(sql), std::move(types));
}

Result<u64> BlockingPipeline::describe(std::string name)
{
  if (auto status = state().queueable(); !status)
    return std::unexpected(status.error());
  return state().pipeline.describe(std::move(name));
}

Result<u64> BlockingPipeline::describe_portal(std::string name)
{
  if (auto status = state().queueable(); !status)
    return std::unexpected(status.error());
  return state().pipeline.describe_portal(std::move(name));
}

Result<u64> BlockingPipeline::close_prepared(std::string name)
{
  if (auto status = state().queueable(); !status)
    return std::unexpected(status.error());
  return state().pipeline.close_prepared(std::move(name));
}

Result<u64> BlockingPipeline::close_portal(std::string name)
{
  if (auto status = state().queueable(); !status)
    return std::unexpected(status.error());
  return state().pipeline.close_portal(std::move(name));
}

Result<u64> BlockingPipeline::sync()
{
  if (auto status = state().queueable(); !status)
    return std::unexpected(status.error());
  return state().pipeline.sync();
}

Result<void> BlockingPipeline::request_flush()
{
  if (auto status = state().queueable(); !status)
    return status;
  return state().pipeline.request_flush();
}

Result<void> BlockingPipeline::flush()
{
  auto &impl = state();
  if (!impl.ctx)
    return std::unexpected(make_error_code(Error::closed));
  if (auto status = impl.queueable(); !status)
    return status;
  if (impl.streaming)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  return impl.ctx->run(impl.pipeline.flush());
}

Result<void> BlockingPipeline::start()
{
  auto &impl = state();
  if (!impl.ctx)
    return std::unexpected(make_error_code(Error::closed));
  if (auto status = impl.queueable(); !status)
    return status;

  auto job = impl.ctx->spawn(impl.pipeline.flush());
  if (!job)
    return std::unexpected(job.error());
  impl.driver.emplace(std::move(*job));
  impl.streaming = false;
  return {};
}

Result<void> BlockingPipeline::send()
{
  auto &impl = state();
  if (!impl.ctx)
    return std::unexpected(make_error_code(Error::closed));
  if (auto status = impl.queueable(); !status)
    return status;

  auto written = impl.ctx->run(impl.pipeline.send());
  if (written)
    impl.sent = true;
  return written;
}

Result<void> BlockingPipeline::receive()
{
  auto &impl = state();
  if (!impl.ctx)
    return std::unexpected(make_error_code(Error::closed));
  if (impl.driver || !impl.sent)
    return std::unexpected(make_error_code(Error::busy));
  if (impl.streaming)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  auto read = impl.ctx->run(impl.pipeline.receive());
  if (read)
    impl.sent = false;
  return read;
}

Result<void> BlockingPipeline::start_receive()
{
  auto &impl = state();
  if (!impl.ctx)
    return std::unexpected(make_error_code(Error::closed));
  if (impl.driver || !impl.sent)
    return std::unexpected(make_error_code(Error::busy));

  auto job = impl.ctx->spawn(impl.pipeline.receive());
  if (!job)
    return std::unexpected(job.error());
  impl.driver.emplace(std::move(*job));
  impl.streaming = false;
  return {};
}

Result<std::optional<PipelineResult>> BlockingPipeline::next()
{
  auto &impl = state();
  if (!impl.driver)
    return impl.pipeline.try_next();

  auto result = impl.ctx->run(impl.pipeline.next());
  if (result && *result)
    return result;

  if (!result)
    impl.driver->cancel();
  auto completed = impl.ctx->run(join_pipeline(std::move(*impl.driver)));
  impl.driver.reset();
  impl.sent = false;
  if (result && !completed)
    return std::unexpected(completed.error());
  return result;
}

Result<void> BlockingPipeline::finish()
{
  if (auto status = state().queueable(); !status)
    return status;
  auto result = state().pipeline.finish();
  if (result)
    state().ctx = nullptr;

  return result;
}

bool BlockingPipeline::aborted() const noexcept
{
  return state().pipeline.aborted();
}

struct BlockingConnection::Impl {
  Result<Context> ctx = Context::create();
  std::optional<Connection> connection;
};

BlockingConnection::BlockingConnection(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

BlockingConnection::BlockingConnection(BlockingConnection &&other) noexcept = default;
BlockingConnection::~BlockingConnection() = default;

BlockingConnection::Impl &BlockingConnection::state() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return *impl_;
}

Context &detail::LargeObjectAccess::context(BlockingConnection &connection) noexcept
{
  return *connection.state().ctx;
}

Connection &detail::LargeObjectAccess::session(BlockingConnection &connection) noexcept
{
  return *connection.state().connection;
}

Result<BlockingConnection> BlockingConnection::connect(Options options, NoticeHandler notices)
{
  detail::OptionsCleanup cleanup{options};
  return connect(std::move(options), std::move(notices), Trace{});
}

Result<BlockingConnection> BlockingConnection::connect(Options options, NoticeHandler notices, Trace trace)
{
  detail::OptionsCleanup cleanup{options};
  Diagnostic diagnostic;
  return connect(std::move(options), diagnostic, std::move(notices), std::move(trace));
}

Result<BlockingConnection> BlockingConnection::connect(Options options, Diagnostic &diagnostic, NoticeHandler notices)
{
  detail::OptionsCleanup cleanup{options};
  return connect(std::move(options), diagnostic, std::move(notices), Trace{});
}

Result<BlockingConnection> BlockingConnection::connect(
  Options options,
  Diagnostic &diagnostic,
  NoticeHandler notices,
  Trace trace)
{
  detail::OptionsCleanup cleanup{options};
  diagnostic = {};
  auto impl = std::make_unique<Impl>();
  if (!impl->ctx)
    return std::unexpected(impl->ctx.error());

  auto connection = impl->ctx->run(pg::connect(std::move(options), diagnostic, std::move(notices), std::move(trace)));
  if (!connection)
    return std::unexpected(connection.error());

  impl->connection.emplace(std::move(*connection));
  return BlockingConnection{std::move(impl)};
}

Result<BlockingConnection> BlockingConnection::connect(Options options, ConnectionReport &report, NoticeHandler notices)
{
  detail::OptionsCleanup cleanup{options};
  return connect(std::move(options), report, std::move(notices), Trace{});
}

Result<BlockingConnection> BlockingConnection::connect(
  Options options,
  ConnectionReport &report,
  NoticeHandler notices,
  Trace trace)
{
  detail::OptionsCleanup cleanup{options};
  report = {};
  auto impl = std::make_unique<Impl>();
  if (!impl->ctx) {
    report.error = impl->ctx.error();
    report.completed = true;
    return std::unexpected(report.error);
  }

  auto connection = impl->ctx->run(pg::connect(std::move(options), report, std::move(notices), std::move(trace)));
  if (!connection)
    return std::unexpected(connection.error());

  impl->connection.emplace(std::move(*connection));
  return BlockingConnection{std::move(impl)};
}

Result<Results> BlockingConnection::query(std::string sql)
{
  return state().ctx->run(state().connection->query(std::move(sql)));
}

Result<std::vector<Outcome>> BlockingConnection::query_outcomes(std::string sql)
{
  return state().ctx->run(state().connection->query_outcomes(std::move(sql)));
}

Result<BlockingExchange> BlockingConnection::exchange(std::string sql, RowOptions rows)
{
  auto session = state().ctx->run(state().connection->exchange(std::move(sql), rows));
  if (!session)
    return std::unexpected(session.error());
  return BlockingExchange{std::make_unique<BlockingExchange::Impl>(*state().ctx, std::move(*session))};
}

Result<void> BlockingConnection::reset(Options options)
{
  detail::OptionsCleanup cleanup{options};
  return state().ctx->run(state().connection->reset(std::move(options)));
}

Result<void> BlockingConnection::reset(Options options, Diagnostic &diagnostic)
{
  detail::OptionsCleanup cleanup{options};
  return state().ctx->run(state().connection->reset(std::move(options), diagnostic));
}

Result<void> BlockingConnection::reset(Options options, ConnectionReport &report)
{
  detail::OptionsCleanup cleanup{options};
  return state().ctx->run(state().connection->reset(std::move(options), report));
}

Result<ResultSet> BlockingConnection::execute(std::string sql, std::vector<Parameter> parameters, Format format)
{
  return state().ctx->run(state().connection->execute(std::move(sql), std::move(parameters), format));
}

Result<Outcome> BlockingConnection::execute_outcome(std::string sql, std::vector<Parameter> parameters, Format format)
{
  return state().ctx->run(state().connection->execute_outcome(std::move(sql), std::move(parameters), format));
}

Result<void> BlockingConnection::prepare(std::string name, std::string sql, std::vector<u32> types)
{
  return state().ctx->run(state().connection->prepare(std::move(name), std::move(sql), std::move(types)));
}

Result<ResultSet> BlockingConnection::execute_prepared(
  std::string name,
  std::vector<Parameter> parameters,
  Format format)
{
  return state().ctx->run(state().connection->execute_prepared(std::move(name), std::move(parameters), format));
}

Result<Outcome> BlockingConnection::execute_prepared_outcome(
  std::string name,
  std::vector<Parameter> parameters,
  Format format)
{
  return state().ctx->run(state().connection->execute_prepared_outcome(std::move(name), std::move(parameters), format));
}

Result<std::vector<Outcome>> BlockingConnection::batch(std::vector<Command> commands)
{
  return state().ctx->run(state().connection->batch(std::move(commands)));
}

Result<BlockingPipeline> BlockingConnection::pipeline()
{
  auto pipeline = state().connection->pipeline();
  if (!pipeline)
    return std::unexpected(pipeline.error());

  return BlockingPipeline{std::make_unique<BlockingPipeline::Impl>(*state().ctx, std::move(*pipeline))};
}

Result<ResultSet> BlockingConnection::describe(std::string name)
{
  return state().ctx->run(state().connection->describe(std::move(name)));
}

Result<ResultSet> BlockingConnection::describe_portal(std::string name)
{
  return state().ctx->run(state().connection->describe_portal(std::move(name)));
}

Result<void> BlockingConnection::close_prepared(std::string name)
{
  return state().ctx->run(state().connection->close_prepared(std::move(name)));
}

Result<Value> BlockingConnection::call_function(u32 function, std::vector<Parameter> parameters, Format format)
{
  return state().ctx->run(state().connection->call_function(function, std::move(parameters), format));
}

Result<CopyFormat> BlockingConnection::start_copy(std::string sql)
{
  return state().ctx->run(state().connection->start_copy(std::move(sql)));
}

Result<void> BlockingConnection::write_copy(std::span<const std::byte> data)
{
  return state().ctx->run(state().connection->write_copy(data));
}

Result<void> BlockingConnection::finish_copy_send()
{
  return state().ctx->run(state().connection->finish_copy_send());
}

Result<std::optional<std::vector<std::byte>>> BlockingConnection::read_copy()
{
  return state().ctx->run(state().connection->read_copy());
}

Result<ResultSet> BlockingConnection::end_copy(std::optional<std::string> error)
{
  return state().ctx->run(state().connection->end_copy(std::move(error)));
}

Result<ResultSet> BlockingConnection::copy_result() const
{
  return state().connection->copy_result();
}

Result<Results> BlockingConnection::copy_results() const
{
  return state().connection->copy_results();
}

Result<void> BlockingConnection::start_rows(std::string sql)
{
  return state().ctx->run(state().connection->start_rows(std::move(sql)));
}

Result<std::optional<Row>> BlockingConnection::read_row()
{
  return state().ctx->run(state().connection->read_row());
}

const std::vector<Column> &BlockingConnection::row_columns() const noexcept
{
  return state().connection->row_columns();
}

Result<ResultSet> BlockingConnection::open_portal(std::string name, std::string sql, std::vector<Parameter> parameters)
{
  return state().ctx->run(state().connection->open_portal(std::move(name), std::move(sql), std::move(parameters)));
}

Result<ResultSet> BlockingConnection::fetch(std::string name, u32 rows)
{
  return state().ctx->run(state().connection->fetch(std::move(name), rows));
}

Result<void> BlockingConnection::close_portal(std::string name)
{
  return state().ctx->run(state().connection->close_portal(std::move(name)));
}

Result<Notification> BlockingConnection::wait_notification()
{
  return state().ctx->run(state().connection->wait_notification());
}

Result<CancelHandle> BlockingConnection::cancel_handle() const
{
  return state().connection->cancel_handle();
}

Result<void> BlockingConnection::finish()
{
  return state().ctx->run(state().connection->finish());
}

Diagnostic BlockingConnection::last_error() const
{
  return state().connection->last_error();
}

Result<Failure> BlockingConnection::last_failure() const
{
  if (!impl_)
    return std::unexpected(make_error_code(Error::closed));

  return impl_->connection->last_failure();
}

Result<NoticeHandler> BlockingConnection::on_notice(NoticeHandler handler)
{
  return state().connection->on_notice(std::move(handler));
}

Result<Trace> BlockingConnection::on_trace(Trace trace)
{
  return state().connection->on_trace(std::move(trace));
}

Result<NotificationHandler> BlockingConnection::on_notification(NotificationHandler handler)
{
  return state().connection->on_notification(std::move(handler));
}

Result<EventId> BlockingConnection::on_event(std::string name, EventHandler handler)
{
  return state().connection->on_event(std::move(name), std::move(handler));
}

Result<void> BlockingConnection::attach_events(ResultSet &result)
{
  return state().connection->attach_events(result);
}

Result<EventData> BlockingConnection::event_data(EventId id) const
{
  return state().connection->event_data(id);
}

Result<void> BlockingConnection::set_event_data(EventId id, EventData data)
{
  return state().connection->set_event_data(id, std::move(data));
}

Result<void> BlockingConnection::close() noexcept
{
  return impl_ ? impl_->connection->close() : Result<void>{};
}

Result<void> BlockingConnection::cancel() noexcept
{
  return impl_ ? impl_->connection->cancel() : Result<void>{};
}

bool BlockingConnection::open() const noexcept
{
  return impl_ && impl_->connection->open();
}

Transaction BlockingConnection::transaction() const noexcept
{
  return impl_ ? impl_->connection->transaction() : Transaction::unknown;
}

PipelineStatus BlockingConnection::pipeline_status() const noexcept
{
  return impl_ ? impl_->connection->pipeline_status() : PipelineStatus::off;
}

u32 BlockingConnection::backend_process() const noexcept
{
  return state().connection->backend_process();
}

ProtocolVersion BlockingConnection::protocol_version() const noexcept
{
  return state().connection->protocol_version();
}

Authentication BlockingConnection::authentication_method() const noexcept
{
  return state().connection->authentication_method();
}

bool BlockingConnection::gss_encrypted() const noexcept
{
  return state().connection->gss_encrypted();
}

Result<ConnectionInfo> BlockingConnection::info() const
{
  if (!impl_)
    return std::unexpected(make_error_code(Error::closed));

  return impl_->connection->info();
}

Result<OptionsInfo> BlockingConnection::configuration() const
{
  if (!impl_)
    return std::unexpected(make_error_code(Error::closed));

  return impl_->connection->configuration();
}

Result<Encoding> BlockingConnection::client_encoding() const noexcept
{
  return state().connection->client_encoding();
}

Result<void> BlockingConnection::set_client_encoding(Encoding encoding)
{
  return state().ctx->run(state().connection->set_client_encoding(encoding));
}

Result<std::string> BlockingConnection::password_verifier(
  std::string user,
  std::string_view password,
  PasswordOptions options)
{
  return state().ctx->run(state().connection->password_verifier(std::move(user), password, options));
}

Result<ResultSet> BlockingConnection::change_password(
  std::string user,
  std::string_view password,
  PasswordOptions options)
{
  return state().ctx->run(state().connection->change_password(std::move(user), password, options));
}

Result<std::optional<std::string>> BlockingConnection::parameter(std::string_view name) const
{
  if (!impl_)
    return std::unexpected(make_error_code(Error::closed));
  return impl_->connection->parameter(name);
}

Result<std::string> BlockingConnection::escape_literal(std::string_view text) const
{
  return state().connection->escape_literal(text);
}

Result<std::string> BlockingConnection::escape_identifier(std::string_view text) const
{
  return state().connection->escape_identifier(text);
}

std::vector<Diagnostic> BlockingConnection::take_notices()
{
  return state().connection->take_notices();
}

std::vector<Notification> BlockingConnection::take_notifications()
{
  return state().connection->take_notifications();
}

} // namespace weave::pg
