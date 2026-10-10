#if defined(WEAVE_USE_POSTGRES)
#include <weave/postgres.hpp>
#elif defined(WEAVE_USE_TLS)
#include <weave/tls.hpp>
#elif defined(WEAVE_USE_SYNC)
#include <weave/sync.hpp>
#include <weave/io.hpp>
#elif defined(WEAVE_USE_TCP)
#include <weave/tcp.hpp>
#elif defined(WEAVE_USE_LOCAL)
#include <weave/local.hpp>
#elif defined(WEAVE_USE_RUNTIME)
#include <weave/runtime.hpp>
#elif defined(WEAVE_USE_IO)
#include <weave/io.hpp>
#else
#include <weave/core.hpp>
#endif
#include <weave/port.hpp>
#if defined(WEAVE_USE_IO) || defined(WEAVE_USE_RUNTIME) || defined(WEAVE_USE_TCP)
#include <weave/address.hpp>
#include <weave/resolve.hpp>
#endif

#if defined(_WINDOWS_) || defined(_WINSOCKAPI_) || defined(_WINSOCK2API_)
#error Public header leaked Windows headers
#endif

#include <array>
#include <cstdio>
#include <cstdlib>

#if defined(WEAVE_USE_TLS) || defined(WEAVE_USE_POSTGRES)
weave::Result<weave::TlsInfo> tls_metadata(const weave::TlsStream<weave::TcpStream> &stream)
{
  return stream.info();
}
#endif

static weave::Task<int> value()
{
  co_return 42;
}

#if defined(WEAVE_USE_RUNTIME) || defined(WEAVE_USE_IO)
static void detach_error(weave::Error) noexcept
{
  std::abort();
}

static weave::Task<void> scoped_detach_values()
{
  weave::detach(value());
  weave::detach(value);
  co_return;
}
#endif

#if defined(WEAVE_USE_TCP)
static weave::Task<void> server(weave::TcpListener &listener)
{
  auto socket = co_await listener.accept();
  std::array<std::byte, 4> buffer;
  co_await socket.read_exactly(buffer);
  co_await socket.write_all(buffer);
}

static weave::Task<void> client(weave::u16 port)
{
  auto socket = co_await weave::tcp::connect("127.0.0.1", port);
  std::array<std::byte, 4> sent{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
  std::array<std::byte, 4> received;
  co_await socket.write_all(sent);
  co_await socket.read_exactly(received);
  if (sent != received)
    co_await weave::fail(std::errc::io_error);
}

static weave::Task<void> discard_client(weave::TcpStream)
{
  co_return;
}

static weave::Task<void> echo_data(weave::TcpStream &socket, std::span<const std::byte> data)
{
  co_await socket.write_all(data);
}
#endif

#if defined(WEAVE_USE_POSTGRES)
static_assert(static_cast<int>(weave::pg::Transaction::idle) == 0);
static_assert(static_cast<int>(weave::pg::Transaction::active) == 1);
static_assert(static_cast<int>(weave::pg::Transaction::failed) == 2);
static_assert(static_cast<int>(weave::pg::Transaction::in_progress) == 3);
static_assert(static_cast<int>(weave::pg::Transaction::unknown) == 4);
static_assert(noexcept(std::declval<const weave::pg::Connection &>().pipeline_status()));
static_assert(noexcept(std::declval<const weave::pg::BlockingConnection &>().pipeline_status()));

weave::pg::PipelineStatus postgres_pipeline_status(const weave::pg::Connection &connection) noexcept
{
  return connection.pipeline_status();
}

weave::pg::PipelineStatus postgres_blocking_pipeline_status(const weave::pg::BlockingConnection &connection) noexcept
{
  return connection.pipeline_status();
}

weave::pg::Transaction postgres_transaction(const weave::pg::Connection &connection) noexcept
{
  return connection.transaction();
}

weave::pg::Transaction postgres_blocking_transaction(const weave::pg::BlockingConnection &connection) noexcept
{
  return connection.transaction();
}

weave::Result<weave::pg::OptionsInfo> postgres_configuration(const weave::pg::Connection &connection)
{
  return connection.configuration();
}

weave::Result<weave::pg::OptionsInfo> postgres_blocking_configuration(const weave::pg::BlockingConnection &connection)
{
  return connection.configuration();
}

std::span<const weave::pg::OptionDescriptor> postgres_option_schema() noexcept
{
  return weave::pg::option_schema();
}

weave::Result<weave::pg::Failure> postgres_failure(const weave::pg::Connection &connection)
{
  return connection.last_failure();
}

weave::Result<weave::pg::Failure> postgres_blocking_failure(const weave::pg::BlockingConnection &connection)
{
  return connection.last_failure();
}

weave::Task<std::vector<weave::pg::Outcome>> postgres_outcomes(weave::pg::Connection &connection)
{
  return connection.query_outcomes("SELECT 1");
}

weave::Task<weave::pg::Connection> postgres_report(weave::pg::Options options, weave::pg::ConnectionReport &report)
{
  return weave::pg::connect(std::move(options), report);
}

weave::Result<weave::pg::BlockingConnection> postgres_blocking_report(
  weave::pg::Options options,
  weave::pg::ConnectionReport &report)
{
  return weave::pg::BlockingConnection::connect(std::move(options), report);
}

weave::Task<void> postgres_reset_report(
  weave::pg::Connection &connection,
  weave::pg::Options options,
  weave::pg::ConnectionReport &report)
{
  return connection.reset(std::move(options), report);
}

weave::Result<void> postgres_blocking_reset_report(
  weave::pg::BlockingConnection &connection,
  weave::pg::Options options,
  weave::pg::ConnectionReport &report)
{
  return connection.reset(std::move(options), report);
}

weave::Result<std::vector<weave::pg::Outcome>> postgres_blocking_outcomes(weave::pg::BlockingConnection &connection)
{
  return connection.query_outcomes("SELECT 1");
}

weave::Task<weave::pg::Outcome> postgres_execute_outcome(weave::pg::Connection &connection)
{
  return connection.execute_outcome("SELECT 1");
}

weave::Task<weave::pg::Outcome> postgres_prepared_outcome(weave::pg::Connection &connection)
{
  return connection.execute_prepared_outcome("package-statement");
}

weave::Result<weave::pg::Outcome> postgres_blocking_execute_outcome(weave::pg::BlockingConnection &connection)
{
  return connection.execute_outcome("SELECT 1");
}

weave::Result<weave::pg::Outcome> postgres_blocking_prepared_outcome(weave::pg::BlockingConnection &connection)
{
  return connection.execute_prepared_outcome("package-statement");
}

weave::Task<weave::pg::ResultSet> postgres_portal_description(weave::pg::Connection &connection)
{
  return connection.describe_portal("package-portal");
}

weave::Result<weave::pg::ResultSet> postgres_blocking_portal_description(weave::pg::BlockingConnection &connection)
{
  return connection.describe_portal("package-portal");
}

weave::Result<weave::pg::ConnectionInfo> postgres_metadata(const weave::pg::Connection &connection)
{
  return connection.info();
}

weave::Result<weave::pg::ConnectionInfo> postgres_blocking_metadata(const weave::pg::BlockingConnection &connection)
{
  return connection.info();
}

weave::Task<weave::pg::ResultSet> postgres_password(weave::pg::Connection &connection)
{
  return connection.change_password("package-user", "package-password");
}

weave::Result<std::string> postgres_blocking_password(weave::pg::BlockingConnection &connection)
{
  return connection.password_verifier("package-user", "package-password");
}

weave::Task<void> postgres_encoding(weave::pg::Connection &connection)
{
  co_await connection.set_client_encoding(weave::pg::Encoding::utf8);
  if (auto encoding = connection.client_encoding(); !encoding)
    co_await weave::fail(encoding.error());
}

weave::Result<void> postgres_blocking_encoding(weave::pg::BlockingConnection &connection)
{
  return connection.set_client_encoding(weave::pg::Encoding::latin1);
}

bool postgres_gss_encrypted(
  const weave::pg::Connection &connection,
  const weave::pg::BlockingConnection &blocking) noexcept
{
  return connection.gss_encrypted() && blocking.gss_encrypted();
}

weave::Result<std::string> postgres_quote()
{
  auto encoding = weave::pg::parse_encoding("ShiftJIS");
  if (!encoding)
    return std::unexpected(encoding.error());
  auto info = weave::pg::encoding_info(*encoding);
  if (!info || !weave::pg::validate_text("text", *encoding) || weave::pg::character_size("text", *encoding) != 1 ||
    weave::pg::character_width("text", *encoding) != 1)
    return std::unexpected(std::make_error_code(std::errc::bad_message));
  return weave::pg::escape_literal("text", *encoding);
}

weave::Task<void> postgres_exchange(weave::pg::Connection &connection)
{
  auto exchange = co_await connection.exchange("COPY (SELECT 42) TO STDOUT; SELECT 42", {.chunk_rows = 2});
  while (auto event = co_await exchange.next()) {
    if (auto format = std::get_if<weave::pg::CopyFormat>(&*event)) {
      if (format->direction == weave::pg::CopyDirection::input)
        co_await exchange.finish_send();
    }
  }
  if (auto status = exchange.finish(); !status)
    co_await weave::fail(status.error());
}

weave::Result<void> postgres_blocking_exchange(weave::pg::BlockingConnection &connection)
{
  auto exchange = connection.exchange("SELECT 42", {.chunk_rows = 2});
  if (!exchange)
    return std::unexpected(exchange.error());
  for (;;) {
    auto event = exchange->next();
    if (!event)
      return std::unexpected(event.error());
    if (!*event)
      return exchange->finish();
  }
}

weave::Result<weave::u64> postgres_streaming_command(weave::pg::Pipeline &pipeline)
{
  return pipeline.execute({"SELECT 42"}, {.chunk_rows = 1});
}

weave::Result<weave::pg::NoticeHandler> postgres_notices(weave::pg::Connection &connection)
{
  return connection.on_notice([](const weave::pg::Diagnostic &) noexcept {
  });
}

weave::Result<std::optional<std::string>> postgres_parameter(
  const weave::pg::Connection &connection,
  std::string_view name)
{
  return connection.parameter(name);
}

weave::Result<std::optional<std::string>> postgres_blocking_parameter(
  const weave::pg::BlockingConnection &connection,
  std::string_view name)
{
  return connection.parameter(name);
}

weave::u32 postgres_server_version_number(const weave::pg::ConnectionInfo &info)
{
  return info.server_version_number;
}

std::string_view postgres_server_options(const weave::pg::ConnectionInfo &info)
{
  return info.server_options;
}

weave::pg::AuthenticationInfo postgres_authentication(const weave::pg::ConnectionInfo &info)
{
  return info.authentication;
}

weave::pg::AuthenticationInfo postgres_attempt_authentication(const weave::pg::ConnectionAttempt &attempt)
{
  return attempt.authentication;
}

bool postgres_password_missing(const weave::pg::ConnectionReport &report)
{
  return report.authentication.password_missing;
}

weave::Task<weave::pg::Connection> postgres_startup_notices(weave::pg::Options options)
{
  return weave::pg::connect(std::move(options), [](const weave::pg::Diagnostic &) noexcept {
  });
}

weave::Result<weave::pg::NoticeHandler> postgres_blocking_notices(weave::pg::BlockingConnection &connection)
{
  return connection.on_notice({});
}

weave::Result<weave::pg::NotificationHandler> postgres_notifications(weave::pg::Connection &connection)
{
  return connection.on_notification([](const weave::pg::Notification &) noexcept {
  });
}

weave::Result<weave::pg::NotificationHandler> postgres_blocking_notifications(weave::pg::BlockingConnection &connection)
{
  return connection.on_notification({});
}

weave::Result<weave::pg::EventId> postgres_events(weave::pg::Connection &connection)
{
  return connection.on_event("package", [](weave::pg::Event &) noexcept -> weave::Result<void> {
    return {};
  });
}

weave::Result<weave::pg::EventId> postgres_blocking_events(weave::pg::BlockingConnection &connection)
{
  return connection.on_event("package", [](weave::pg::Event &) noexcept -> weave::Result<void> {
    return {};
  });
}

weave::Result<weave::pg::EventData> postgres_result_data(const weave::pg::ResultSet &result, weave::pg::EventId id)
{
  auto copy = result;
  return copy.event_data(id);
}

weave::Task<void> postgres_pipeline_consumer(weave::pg::Pipeline &pipeline)
{
  while (auto event = co_await pipeline.next()) {
    if (!event->complete && !event->outcome.result)
      co_await weave::fail(std::errc::bad_message);
  }
}

weave::Task<void> postgres_pipeline_send(weave::pg::Pipeline &pipeline)
{
  co_await pipeline.send();
}

weave::Task<void> postgres_pipeline_receive(weave::pg::Pipeline &pipeline)
{
  co_await pipeline.receive();
}

weave::Result<void> postgres_blocking_pipeline(weave::pg::BlockingPipeline &pipeline)
{
  auto queued = pipeline.execute({"SELECT 42"}, {.chunk_rows = 1});
  if (!queued)
    return std::unexpected(queued.error());
  if (auto barrier = pipeline.sync(); !barrier)
    return std::unexpected(barrier.error());
  if (auto written = pipeline.send(); !written)
    return written;
  if (auto started = pipeline.start_receive(); !started)
    return started;
  for (;;) {
    auto event = pipeline.next();
    if (!event)
      return std::unexpected(event.error());
    if (!*event)
      return pipeline.finish();
  }
}

weave::Result<void> postgres_blocking_receive(weave::pg::BlockingPipeline &pipeline)
{
  return pipeline.receive();
}
#endif

int main()
{
  if (weave::parse_port("8080") != 8080)
    return 5;

#if defined(WEAVE_USE_POSTGRES)
  // Volatile loads retain symbol references even in optimized, network-free consumers.
  auto volatile attach = &weave::pg::Connection::attach_events;
  auto volatile blocking_attach = &weave::pg::BlockingConnection::attach_events;
  auto volatile request_flush = &weave::pg::Pipeline::request_flush;
  auto volatile blocking_request_flush = &weave::pg::BlockingPipeline::request_flush;
  auto volatile exchange = &weave::pg::Connection::exchange;
  auto volatile blocking_exchange = &weave::pg::BlockingConnection::exchange;
  if (!attach || !blocking_attach || !request_flush || !blocking_request_flush || !exchange || !blocking_exchange)
    return 2;

  weave::pg::Options configured{.user = "package-owner"};
  auto configuration = configured.info();
  configured.user = "changed";
  if (configuration.user != "package-owner" || configuration.database != "package-owner" ||
    !configuration.tls_options || configuration.tls_context)
    return 2;
  auto defaults = weave::pg::Options::defaults(
    {.environment = false, .user_files = false, .ldap = false, .system_files = false});
  if (!defaults || !defaults->origin || defaults->origin->sources.environment || defaults->origin->sources.user_files ||
    defaults->origin->sources.system_files || defaults->origin->sources.ldap)
    return 2;

  if (weave::pg::result_kind_name(weave::pg::ResultKind::tuples) != "tuples")
    return 2;
  weave::pg::ResultSet columns;
  columns.columns.push_back({.name = "DisplayName"});
  auto column = columns.column_index("\"DisplayName\"");
  if (!column || !*column || **column != 0)
    return 2;
  auto missing_column = columns.column_index("displayname");
  if (!missing_column || *missing_column)
    return 2;

  columns.kind = weave::pg::ResultKind::tuples;
  columns.command = "SELECT 1";
  columns.parameter_types = {17};
  columns.suspended = true;
  columns.rows.push_back({{std::string{"a\0b", 3}, weave::pg::Format::binary}});
  auto schema = columns.copy({.rows = false, .observers = false});
  auto data = columns.copy({.columns = false, .observers = false});
  auto metadata = columns.copy({.columns = false, .rows = false, .observers = false});
  if (schema.columns.size() != 1 || !schema.rows.empty() || data.columns.size() != 1 || data.rows.size() != 1 ||
    !metadata.columns.empty() || !metadata.rows.empty() || metadata.kind != columns.kind ||
    metadata.command != columns.command || metadata.parameter_types != columns.parameter_types || !metadata.suspended)
    return 2;
  data.rows.front().front().data = "changed";
  if (columns.rows.front().front().bytes() != std::string_view{"a\0b", 3})
    return 2;

  weave::pg::Diagnostic diagnostic{{{'S', "ERROR"}, {'C', "42601"}, {'M', "package"}, {'P', "2"}}};
  auto formatted = diagnostic.format();
  if (!formatted || *formatted != "ERROR:  package at character 2\n")
    return 2;
  auto cursor = diagnostic.format({.verbosity = weave::pg::DiagnosticVerbosity::standard, .query = "abc"});
  if (!cursor || *cursor != "ERROR:  package\nLINE 1: abc\n         ^\n")
    return 2;
  weave::pg::PipelineResult barrier;
  barrier.kind = weave::pg::PipelineKind::sync;
  barrier.transaction = weave::pg::Transaction::failed;
  if (weave::pg::status_name(barrier) != "pipeline_sync" || weave::pg::status_name(diagnostic) != "diagnostic")
    return 2;
  if (weave::pg::status_name(weave::pg::sql_error("22012")) != "sql_error")
    return 2;
  auto verifier = weave::pg::password_verifier("package-user", "package-password");
  if (!verifier || !verifier->starts_with("SCRAM-SHA-256$4096:"))
    return 2;

  auto ctx = weave::Context::create();
  if (!ctx)
    return 1;

  auto pending = weave::pg::connect({.user = "package-test"});
  auto invalid = ctx->run(weave::pg::connect({}));
  if (invalid || invalid.error() != std::errc::invalid_argument)
    return 2;

  weave::pg::ConnectionReport report;
  auto reported = ctx->run(weave::pg::connect({}, report));
  if (reported || reported.error() != std::errc::invalid_argument || !report.completed ||
    report.error != reported.error() || report.attempts.size() != 1 ||
    report.attempts[0].stage != weave::pg::ConnectionStage::validation)
    return 2;
  auto history = report.format();
  if (!history || history->find("validation after ") == std::string::npos)
    return 2;
  if (report.current.stage != weave::pg::ConnectionStage::validation || report.current.error != reported.error() ||
    report.current.elapsed.count() < 0)
    return 2;
  auto compatible = weave::pg::Options::load(
    "user=package-test",
    {.environment = false, .user_files = false, .system_files = false, .libpq_compatibility = true});
  if (!compatible || compatible->tls_mode != weave::pg::TlsMode::prefer)
    return 2;
  weave::pg::Failure failure{reported.error(), {}};
  if (auto text = failure.format(); !text || text->empty())
    return 2;

  auto protected_options = weave::pg::Options::parse("user=package-test gssencmode=require");
  if (!protected_options || protected_options->gss_encryption != weave::pg::GssEncryption::require)
    return 2;
  auto no_provider = ctx->run(weave::pg::connect(std::move(*protected_options)));
  if (no_provider || no_provider.error() != std::errc::operation_not_supported)
    return 2;

  auto oauth = weave::pg::OAuthProvider::create(
    [](weave::pg::OAuthRequest request) noexcept -> weave::Task<weave::pg::OAuthToken> {
      if (request.user != "package-test" || !request.client_secret ||
        request.client_secret->value() != "package-client-secret")
        co_await weave::fail(std::errc::invalid_argument);
      auto token = weave::pg::OAuthToken::parse("package-test-token");
      if (!token)
        co_await weave::fail(token.error());
      co_return std::move(*token);
    },
    [](const weave::pg::OAuthRequest &request) noexcept -> weave::Result<std::optional<weave::pg::OAuthToken>> {
      if (request.user != "package-test")
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      auto token = weave::pg::OAuthToken::parse("package-cached-token");
      if (!token)
        return std::unexpected(token.error());
      return std::optional{std::move(*token)};
    });
  if (!oauth)
    return 2;
  auto secret = weave::pg::Options::parse("oauth_client_secret=package-client-secret");
  if (!secret || !secret->oauth || !secret->oauth->client_secret)
    return 2;
  auto snapshot = secret->oauth->client_secret;
  auto token = ctx->run(oauth->request({.user = "package-test", .client_secret = snapshot}));
  if (!token || token->value() != "package-test-token")
    return 2;
  auto cached = oauth->cached_token({.user = "package-test"});
  if (!cached || !*cached || (*cached)->value() != "package-cached-token")
    return 2;

  auto error = weave::pg::sql_error("23505");
  return weave::pg::sqlstate(error) == "23505" ? 0 : 3;
#elif defined(WEAVE_USE_LOCAL)
  auto ctx = weave::Context::create();
  if (!ctx)
    return 1;
  auto invalid = ctx->run(weave::local::connect(std::string{}));
  if (invalid || invalid.error() != std::errc::invalid_argument)
    return 2;
  auto pending = weave::local::listen("@package-test");
  return 0;
#elif defined(WEAVE_USE_TLS)
  auto tls = weave::TlsContext::client();
  if (!tls)
    return 1;

  // Instantiate the transport adapter without making an external connection.
  auto pending = weave::tls::connect(*tls, "localhost", 443);
  return 0;
#elif defined(WEAVE_USE_SYNC)
  auto ctx = weave::Context::create();
  if (!ctx)
    return 1;

  weave::Semaphore semaphore(1);
  if (!ctx->run(semaphore.acquire()))
    return 2;

  weave::Channel<int> channel(1);
  if (!ctx->run(channel.send(42)))
    return 3;

  channel.close();
  auto value = ctx->run(channel.receive());
  return value && *value && **value == 42 ? 0 : 4;
#elif defined(WEAVE_USE_TCP)
  auto context = weave::Context::create();
  if (!context)
    return 1;
  auto listener = weave::tcp::listen(*context, "127.0.0.1", 0);
  if (!listener)
    return 2;
  auto port = listener->local_port();
  if (port == 0)
    return 3;
  auto result = context->run(weave::when_all(server(*listener), client(port)));
  if (!result)
    return 4;
  auto implicit = context->run(weave::tcp::listen("127.0.0.1", 0));
  if (!implicit || implicit->local_port() == 0)
    return 5;
  // Instantiate the server helper against installed TCP/IO headers, without Runtime.
  auto invalid = context->run(weave::tcp::serve("invalid", 0, {.no_delay = true}, discard_client));
  if (invalid || invalid.error() != std::errc::invalid_argument)
    return 6;
  auto buffered = context->run(weave::tcp::serve("invalid", 0, {}, weave::tcp::on_data(echo_data)));
  if (buffered || buffered.error() != std::errc::invalid_argument)
    return 7;
  return 0;
#elif defined(WEAVE_USE_RUNTIME)
  for (auto scheduler : {weave::Scheduler::worker_affine, weave::Scheduler::work_stealing}) {
    auto runtime = weave::Runtime::create({.workers = 2, .scheduler = scheduler});
    if (!runtime)
      return 1;
    if (runtime->run(value()) != 42 || runtime->run(value) != 42)
      return 2;
    auto inherited = runtime->run(scoped_detach_values());
    if (!inherited)
      return 2;
    runtime->detach(
      [](weave::Context &ctx) -> weave::Task<void> {
        co_await ctx.yield();
      },
      detach_error);
    runtime->detach(value(), detach_error);
    runtime->detach(value, detach_error);
    runtime->detach_on(0, value(), detach_error);
    runtime->detach_on(0, value);
    auto direct = runtime->spawn(value());
    auto pinned = runtime->spawn_on(0, value());
    auto deferred = runtime->spawn_on(0, value);
    if (!direct || !pinned || !deferred || std::move(*direct).get() != 42 || std::move(*pinned).get() != 42 ||
      std::move(*deferred).get() != 42)
      return 2;
    auto task = runtime->spawn([](weave::Context &context) -> weave::Task<int> {
      co_await context.yield();
      co_return co_await value();
    });
    if (!task || std::move(*task).get() != 42)
      return 2;
    runtime->join();
  }
  return 0;
#elif defined(WEAVE_USE_IO)
  auto context = weave::Context::create();
  if (!context)
    return 1;
  auto address = weave::IpAddress::parse("::1");
  if (!address || address->to_string() != "::1")
    return 4;
  auto endpoints = context->run(weave::resolve("localhost", 8080));
  if (!endpoints || endpoints->empty() || endpoints->front().port != 8080)
    return 4;
  auto inherited = context->run(scoped_detach_values());
  if (!inherited)
    return 2;
  context->detach(
    [](weave::Context &ctx) -> weave::Task<void> {
      co_await ctx.yield();
    },
    detach_error);
  context->detach(value(), detach_error);
  context->detach(value, detach_error);
  auto direct = context->spawn(value());
  auto deferred = context->spawn(value);
  if (!direct || !deferred || context->run(std::move(*direct).as_task()) != 42 ||
    context->run(std::move(*deferred).as_task()) != 42)
    return 2;
  auto task = context->spawn([](weave::Context &ctx) -> weave::Task<int> {
    co_await ctx.yield();
    co_return co_await value();
  });
  if (!task)
    return 2;
  return context->run(std::move(*task).as_task()) == 42 ? 0 : 3;
#else
  int observed = 0;
  auto task = value().on_error([&](weave::Error) noexcept {
    ++observed;
  });
  weave::detail::TaskAccess::start(task);
  if (!weave::detail::TaskAccess::done(task))
    return 1;
  return weave::detail::TaskAccess::take(task) == 42 && observed == 0 ? 0 : 2;
#endif
}
