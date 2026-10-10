#pragma once

#include <weave/tls.hpp>
#include <weave/postgres/configuration.hpp>
#include <weave/postgres/encoding.hpp>
#include <weave/postgres/password.hpp>
#include <weave/postgres/gss.hpp>
#include <weave/postgres/oauth.hpp>
#include <weave/postgres/trace.hpp>
#include <weave/postgres/events.hpp>
#include <weave/postgres/detail/result_storage.hpp>
#include <charconv>
#include <bit>
#include <memory>
#include <variant>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <span>
#include <vector>
#include <array>

namespace weave::pg {

namespace detail {

struct LargeObjectAccess;
struct OwnedOptions;
struct ResultEvents;
struct EventAccess;
struct ResultAccess;

} // namespace detail

enum class Error {
  protocol = 1,
  authentication,
  unsupported_authentication,
  resource_limit,
  closed,
  busy,
  unexpected_copy,
  target_session,
};

std::error_code make_error_code(Error error) noexcept;
std::error_code sql_error(std::string_view sqlstate) noexcept;
std::string sqlstate(std::error_code error);

enum class Format : u16 {
  text,
  binary
};
enum class ChannelBinding {
  disable,
  prefer,
  require
};

enum class TlsNegotiation {
  postgres,
  direct
};

enum class TlsMode {
  disable,
  allow,
  prefer,
  require,
  verify_ca,
  verify_full
};
enum class Transaction {
  idle,
  // An open SQL transaction, not an executing command.
  active,
  failed,
  // Pending protocol work; the last ReadyForQuery state is not definitive yet.
  in_progress,
  unknown
};

enum class PipelineStatus {
  off,
  on,
  aborted
};

enum class TargetSession {
  any,
  read_write,
  read_only,
  primary,
  standby,
  prefer_standby
};

enum class Replication {
  disabled,
  physical,
  database
};

enum class HostBalance {
  ordered,
  random
};

enum class ProtocolVersion : u32 {
  v30 = 196608,
  v32 = 196610
};

enum class Authentication {
  none,
  password,
  md5,
  scram_sha256,
  gss,
  sspi,
  oauth
};

struct AuthenticationPolicy {
  std::vector<Authentication> methods;
  bool exclude = false;
};

struct AuthenticationInfo {
  Authentication method = Authentication::none;
  // Challenge facts, not proof that a password was sent or accepted.
  bool password_requested = false;
  bool password_missing = false;
  bool complete = false;
};

struct ConnectionInfo {
  std::string host;
  u16 port = 0;
  std::optional<Endpoint> endpoint;
  std::optional<std::string> local_address;
  std::optional<u64> peer_user;
  std::string database;
  std::string user;
  std::string server_options;
  std::string server_version;
  u32 server_version_number = 0;
  u32 backend_process = 0;
  ProtocolVersion protocol_version = ProtocolVersion::v30;
  AuthenticationInfo authentication;
  Transaction transaction = Transaction::idle;
  bool gss_encrypted = false;
  std::optional<TlsInfo> tls;
};

struct Host {
  std::string name = "localhost";
  u16 port = 5432;
  std::optional<IpAddress> address;
  std::optional<std::string> password;
};

struct Limits {
  std::size_t message_bytes = 16 * 1024 * 1024;
  std::size_t result_bytes = 128 * 1024 * 1024;
  std::size_t queued_notifications = 1024;
  u32 scram_iterations = 1000000;
  std::size_t pipeline_commands = 1024;
};

struct HostInfo {
  std::string name;
  u16 port = 5432;
  std::optional<IpAddress> address;
};

struct TlsOptionsInfo {
  std::string ca_file;
  std::vector<std::string> alpn;
  TlsVersion min_version = TlsVersion::tls12;
  TlsVersion max_version = TlsVersion::tls13;
  std::string certificate_file;
  std::string private_key_file;
  std::string ca_directory;
  std::string crl_file;
  std::string crl_directory;
  TlsRevocation revocation = TlsRevocation::none;
  TlsOcsp ocsp = TlsOcsp::none;
  bool session_resumption = false;
  std::chrono::seconds session_lifetime{600};
  std::chrono::seconds ocsp_max_age{3600};
  std::chrono::seconds ocsp_clock_skew{60};
  TlsLimits limits;
  TlsCipherPolicy ciphers;
  bool private_key_password_provider = false;
  TlsPrivateKeyFormat private_key_format = TlsPrivateKeyFormat::pem;
  TlsVerification verification = TlsVerification::hostname;
  bool key_logging = false;
};

struct OAuthOptionsInfo {
  std::string issuer;
  std::string client_id;
  std::optional<std::string> scope;
  bool provider = false;
  std::chrono::milliseconds acquisition_timeout{std::chrono::minutes{30}};
};

struct OptionsInfo {
  std::string host;
  u16 port = 5432;
  std::string user;
  std::string database;
  std::string application_name;
  bool tls_context = false;
  bool plaintext = false;
  bool allow_cleartext_password = false;
  bool allow_md5_password = false;
  ChannelBinding channel_binding = ChannelBinding::prefer;
  std::chrono::milliseconds connect_timeout{30000};
  Limits limits;
  std::vector<HostInfo> hosts;
  TargetSession target_session = TargetSession::any;
  std::string server_options;
  std::optional<TlsOptionsInfo> tls_options;
  std::string client_encoding;
  std::vector<std::pair<std::string, std::string>> settings;
  Replication replication = Replication::disabled;
  TcpKeepAliveOptions keep_alive;
  std::chrono::milliseconds tcp_user_timeout{0};
  HostBalance host_balance = HostBalance::ordered;
  AuthenticationPolicy authentication;
  ProtocolVersion min_protocol = ProtocolVersion::v30;
  ProtocolVersion max_protocol = ProtocolVersion::v32;
  std::optional<u64> required_peer_user;
  bool gss_context = false;
  std::string gss_service;
  bool gss_delegation = false;
  bool gss_mutual = true;
  GssEncryption gss_encryption = GssEncryption::disable;
  std::optional<OAuthOptionsInfo> oauth;
  std::optional<ConfigurationOrigin> origin;
  TlsNegotiation tls_negotiation = TlsNegotiation::postgres;
  bool server_name_indication = true;
  TlsCertificateMode client_certificate = TlsCertificateMode::allow;
  TlsMode tls_mode = TlsMode::verify_full;
};

class ScramKey {
  std::array<std::byte, 32> bytes_{};

public:
  static Result<ScramKey> parse(std::string_view base64);
  explicit ScramKey(std::span<const std::byte, 32> bytes) noexcept;
  ScramKey(const ScramKey &) = default;
  ScramKey &operator=(const ScramKey &) = default;
  ScramKey(ScramKey &&other) noexcept;
  ScramKey &operator=(ScramKey &&other) noexcept;
  ~ScramKey();

  std::span<const std::byte, 32> bytes() const noexcept
  {
    return bytes_;
  }
};

struct Options {
  std::string host = "localhost";
  u16 port = 5432;
  std::string user;
  std::string database;
  std::string password;
  std::string application_name = "weave";
  std::optional<TlsContext> tls;
  bool plaintext = false;
  bool allow_cleartext_password = false;
  bool allow_md5_password = false;
  ChannelBinding channel_binding = ChannelBinding::prefer;
  std::chrono::milliseconds connect_timeout{30000};
  Limits limits;
  std::vector<Host> hosts;
  TargetSession target_session = TargetSession::any;
  std::string server_options;
  std::optional<TlsClientOptions> tls_options;
  std::string client_encoding = "UTF8";
  std::vector<std::pair<std::string, std::string>> settings;
  Replication replication = Replication::disabled;
  TcpKeepAliveOptions keep_alive;
  std::chrono::milliseconds tcp_user_timeout{0};
  HostBalance host_balance = HostBalance::ordered;
  AuthenticationPolicy authentication;
  ProtocolVersion min_protocol = ProtocolVersion::v30;
  ProtocolVersion max_protocol = ProtocolVersion::v32;
  // Linux OS UID, checked before startup on every local connection and cancellation connection.
  std::optional<u64> required_peer_user;
  std::optional<ScramKey> scram_client_key;
  std::optional<ScramKey> scram_server_key;
  std::optional<GssContext> gss;
  std::string gss_service = "postgres";
  bool gss_delegation = false;
  bool gss_mutual = true;
  GssEncryption gss_encryption = GssEncryption::disable;
  std::optional<OAuthOptions> oauth;
  std::optional<ConfigurationOrigin> origin;
  TlsNegotiation tls_negotiation = TlsNegotiation::postgres;
  bool server_name_indication = true;
  TlsCertificateMode client_certificate = TlsCertificateMode::allow;
  TlsMode tls_mode = TlsMode::verify_full;

  OptionsInfo info() const;
  static Result<OptionsInfo> defaults(ConfigSources sources = {});
  static Result<Options> parse(std::string_view connection_string);
  static Result<Options> load(std::string_view connection_string = {}, ConfigSources sources = {});
};

enum class DiagnosticVerbosity {
  terse,
  standard,
  verbose,
  sqlstate
};

enum class DiagnosticContext {
  never,
  errors,
  always
};

struct DiagnosticFormat {
  DiagnosticVerbosity verbosity = DiagnosticVerbosity::terse;
  DiagnosticContext context = DiagnosticContext::never;
  Encoding encoding = Encoding::utf8;
  std::optional<std::string_view> query;
  std::size_t input_bytes = 1024 * 1024;
  std::size_t output_bytes = 16384;
};

struct Diagnostic {
  std::vector<std::pair<char, std::string>> fields;

  std::string_view field(char code) const noexcept;
  std::string_view message() const noexcept;
  std::string_view sqlstate() const noexcept;
  Result<std::string> format(DiagnosticFormat options = {}) const;
};

struct Failure {
  std::error_code error;
  Diagnostic diagnostic;

  Result<std::string> format(DiagnosticFormat options = {}) const;
};

enum class ConnectionStage {
  validation,
  resolution,
  transport,
  socket_options,
  peer_identity,
  gss,
  tls,
  startup,
  authentication,
  target_session,
  oauth
};

struct ConnectionAttempt {
  std::string host;
  u16 port = 0;
  std::optional<Endpoint> endpoint;
  ConnectionStage stage = ConnectionStage::validation;
  AuthenticationInfo authentication;
  std::error_code error;
  Diagnostic diagnostic;
  bool diagnostic_truncated = false;
  std::chrono::microseconds elapsed{0};
  std::array<std::chrono::microseconds, 11> stage_times{};
};

struct ConnectionReport {
  static constexpr std::size_t max_attempts = 4096;
  static constexpr std::size_t max_bytes = 1024 * 1024;

  // Failed attempts only, in execution order. A successful fallback retains earlier failures.
  std::vector<ConnectionAttempt> attempts;
  // Latest attempt, including successful startup and when history is truncated.
  AuthenticationInfo authentication;
  std::error_code error;
  bool completed = false;
  bool truncated = false;
  // Inspect only on the executing Context, never concurrently from another thread.
  ConnectionAttempt current;

  Result<std::string> format(DiagnosticFormat options = {}) const;
};

using NoticeHandler = std::move_only_function<void(const Diagnostic &) noexcept>;

struct Column {
  detail::ResultText name;
  u32 table = 0;
  i16 attribute = 0;
  u32 type = 0;
  i16 type_size = 0;
  i32 modifier = 0;
  Format format = Format::text;
};

struct Value {
  std::optional<detail::ResultText> data;
  Format format = Format::text;

  bool is_null() const noexcept;
  std::string_view bytes() const noexcept;

  template <class T>
    requires(std::is_integral_v<T> && !std::same_as<T, bool>)
  Result<T> integer() const noexcept
  {
    if (format != Format::text)
      return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    if (!data)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    T value{};
    auto parsed = std::from_chars(data->data(), data->data() + data->size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != data->data() + data->size())
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    return value;
  }

  template <class T>
    requires(std::is_integral_v<T> && !std::same_as<T, bool> && sizeof(T) <= 8)
  Result<T> binary_integer() const noexcept
  {
    if (format != Format::binary || !data || data->size() != sizeof(T))
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    std::make_unsigned_t<T> value = 0;
    for (auto byte : *data)
      value = static_cast<std::make_unsigned_t<T>>((value << 8) | static_cast<unsigned char>(byte));

    return std::bit_cast<T>(value);
  }
};

using Row = detail::ResultList<Value>;

enum class ResultKind {
  uninitialized,
  empty_query,
  command,
  tuples,
  description,
  row_chunk,
  acknowledgment
};

std::string_view result_kind_name(ResultKind kind) noexcept;

struct ResultCopyOptions {
  bool columns = true;
  bool rows = true;
  bool observers = true;
};

enum class ResultLayout {
  table,
  delimited,
  html
};

struct ResultFormat {
  ResultLayout layout = ResultLayout::table;
  Encoding encoding = Encoding::utf8;
  bool headers = true;
  bool row_count = true;
  bool expanded = false;
  std::string_view separator = "|";
  std::string_view null_text;
  std::string_view caption;
  std::span<const std::string_view> headings;
  std::size_t input_bytes = 1024 * 1024;
  std::size_t output_bytes = 1024 * 1024;
};

struct ResultSet {
private:
  detail::ResultArena schema_storage_;
  std::unique_ptr<detail::ResultEvents> events_;
  friend struct detail::EventAccess;
  friend struct detail::ResultAccess;

  explicit ResultSet(detail::ResultHeap heap);

public:
  ResultKind kind = ResultKind::uninitialized;
  detail::ResultList<Column> columns;
  detail::ResultList<Row> rows;
  detail::ResultText command;
  detail::ResultList<u32> parameter_types;
  bool suspended = false;

  ResultSet();
  ResultSet(const ResultSet &other);
  ResultSet(ResultSet &&other) noexcept;
  ResultSet &operator=(const ResultSet &other);
  ResultSet &operator=(ResultSet &&other) noexcept;
  ~ResultSet();

  // Rows imply columns; scalar/command/parameter metadata is always preserved.
  ResultSet copy(ResultCopyOptions options = {}) const;
  // Retained requests for distinct reachable pools, not RSS or exclusive ownership.
  // Synchronous inspection traverses fields and may allocate temporary bookkeeping.
  std::size_t memory_size() const;
  Result<std::optional<std::size_t>> column_index(
    std::string_view identifier,
    Encoding encoding = Encoding::utf8,
    std::size_t limit = 65536) const;
  Result<u64> affected_rows() const noexcept;
  // Zero means no inserted OID. The text view borrows the command tag.
  Result<u32> inserted_oid() const noexcept;
  Result<std::string_view> inserted_oid_text() const noexcept;
  // Owning bounded output; never launches a pager or writes to a file.
  Result<std::string> format(ResultFormat options = {}) const;
  Result<EventData> event_data(EventId id) const;
  Result<void> set_event_data(EventId id, EventData data);
};

using Results = std::vector<ResultSet>;

struct Parameter {
  std::optional<std::string> data;
  u32 type = 0;
  Format format = Format::text;
};

struct Notification {
  u32 process = 0;
  std::string channel;
  std::string payload;
};

using NotificationHandler = std::move_only_function<void(const Notification &) noexcept>;

struct Command {
  std::string sql;
  std::vector<Parameter> parameters;
  Format format = Format::text;
};

struct Outcome {
  std::optional<ResultSet> result;
  Diagnostic error;
  bool aborted = false;

  // Explicit application-created diagnostic value, not a successful wire ResultSet.
  [[nodiscard]] static Result<Outcome> failure(Diagnostic diagnostic, std::size_t limit = 1024 * 1024);
};

enum class CopyDirection {
  input,
  output,
  both
};

struct CopyFormat {
  Format format = Format::text;
  std::vector<Format> columns;
  CopyDirection direction = CopyDirection::input;
};

struct CopyDone {};

using ExchangeEvent = std::variant<ResultSet, CopyFormat, std::vector<std::byte>, CopyDone>;

struct RowOptions {
  u32 chunk_rows = 0;
};

class Exchange {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit Exchange(std::unique_ptr<Impl> impl) noexcept;
  Impl &state() const noexcept;
  friend class Connection;

public:
  Exchange(Exchange &&other) noexcept;
  Exchange(const Exchange &) = delete;
  ~Exchange();

  Task<std::optional<ExchangeEvent>> next();
  Task<void> write(std::span<const std::byte> data);
  Task<void> finish_send(std::optional<std::string> error = std::nullopt);
  Result<void> finish();
};

enum class PipelineKind {
  execute,
  prepare,
  describe,
  close,
  sync
};

struct PipelineResult {
  u64 id = 0;
  PipelineKind kind = PipelineKind::execute;
  Outcome outcome;
  std::optional<Transaction> transaction;
  bool complete = true;
};

std::string_view status_name(const ResultSet &result) noexcept;
std::string_view status_name(const Diagnostic &diagnostic) noexcept;
std::string_view status_name(const Outcome &outcome) noexcept;
std::string_view status_name(const CopyFormat &format) noexcept;
std::string_view status_name(const ExchangeEvent &event) noexcept;
std::string_view status_name(const PipelineResult &result) noexcept;
std::string_view status_name(std::error_code error) noexcept;

class Pipeline {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  struct Borrow {
    Impl *impl = nullptr;

    explicit Borrow(Impl &owner) noexcept;
    Borrow(Borrow &&other) noexcept;
    Borrow(const Borrow &) = delete;
    ~Borrow();
  };

  explicit Pipeline(std::unique_ptr<Impl> impl) noexcept;
  Impl &state() const noexcept;
  friend class Connection;

public:
  Pipeline(Pipeline &&other) noexcept;
  Pipeline(const Pipeline &) = delete;
  ~Pipeline();

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

  Task<void> flush();
  Task<void> send();
  Task<void> receive();
  Task<std::optional<PipelineResult>> next();
  Result<std::optional<PipelineResult>> try_next();
  Result<void> finish();
  bool aborted() const noexcept;
};

enum class ServerStatus {
  accepting,
  rejecting,
  no_response
};

Task<ServerStatus> ping(Options options);

class Connection;
Task<Connection> connect(Options options, NoticeHandler notices = {});
Task<Connection> connect(Options options, Diagnostic &diagnostic, NoticeHandler notices = {});
Task<Connection> connect(Options options, NoticeHandler notices, Trace trace);
Task<Connection> connect(Options options, Diagnostic &diagnostic, NoticeHandler notices, Trace trace);
Task<Connection> connect(Options options, ConnectionReport &report, NoticeHandler notices = {});
Task<Connection> connect(Options options, ConnectionReport &report, NoticeHandler notices, Trace trace);

class CancelHandle {
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  friend class Connection;

  explicit CancelHandle(std::shared_ptr<const Impl> impl) noexcept;

public:
  // The returned Task owns the snapshot; it does not borrow this handle.
  Task<void> request() const;
  Result<void> request_blocking() const;
};

class Connection {
  struct Impl;
  struct NoticeReceiver;
  struct NotificationReceiver;
  struct TraceReceiver;
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
  Impl &state() const noexcept;
  static Task<Connection> establish(
    detail::OwnedOptions options,
    Diagnostic *diagnostic,
    std::shared_ptr<NoticeReceiver> notices,
    std::shared_ptr<TraceReceiver> trace,
    std::shared_ptr<NotificationReceiver> notifications = {},
    ConnectionReport *report = nullptr);
  static Task<Connection> establish_reported(
    detail::OwnedOptions options,
    ConnectionReport &report,
    std::shared_ptr<NoticeReceiver> notices,
    std::shared_ptr<TraceReceiver> trace);
  static Task<ServerStatus> probe(detail::OwnedOptions options);
  Task<void> reset_impl(
    detail::OwnedOptions options,
    Diagnostic *diagnostic,
    Borrow borrow,
    ConnectionReport *report = nullptr);
  Task<void> reset_reported(detail::OwnedOptions options, ConnectionReport &report, Borrow borrow);
  Task<Exchange> begin_exchange(std::string sql, RowOptions rows, Borrow borrow);
  friend Task<Connection> connect(Options, NoticeHandler);
  friend Task<Connection> connect(Options, Diagnostic &, NoticeHandler);
  friend Task<Connection> connect(Options, NoticeHandler, Trace);
  friend Task<Connection> connect(Options, Diagnostic &, NoticeHandler, Trace);
  friend Task<Connection> connect(Options, ConnectionReport &, NoticeHandler);
  friend Task<Connection> connect(Options, ConnectionReport &, NoticeHandler, Trace);
  friend Task<ServerStatus> ping(Options);
  friend struct detail::LargeObjectAccess;
  friend class Pipeline;
  friend class Exchange;

  template <class F>
  static Task<void> each_row(Connection &connection, [[maybe_unused]] Borrow borrow, std::string sql, F handler)
  {
    struct Cleanup {
      Connection &connection;
      bool completed = false;

      ~Cleanup()
      {
        if (!completed)
          static_cast<void>(connection.close());
      }
    } cleanup{connection};

    co_await connection.start_rows(std::move(sql));
    while (auto row = co_await connection.read_row()) {
      const auto &columns = connection.row_columns();
      co_await std::invoke(handler, std::as_const(columns), *row);
    }

    cleanup.completed = true;
  }

public:
  Connection(Connection &&other) noexcept;
  Connection(const Connection &) = delete;
  ~Connection();

  Task<Results> query(std::string sql);
  Task<std::vector<Outcome>> query_outcomes(std::string sql);
  Task<Exchange> exchange(std::string sql, RowOptions rows = {});
  Task<ResultSet> execute(std::string sql, std::vector<Parameter> parameters = {}, Format format = Format::text);
  Task<Outcome> execute_outcome(std::string sql, std::vector<Parameter> parameters = {}, Format format = Format::text);
  Task<void> prepare(std::string name, std::string sql, std::vector<u32> types = {});
  Task<ResultSet> execute_prepared(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  Task<Outcome> execute_prepared_outcome(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  Task<ResultSet> describe(std::string name);
  Task<ResultSet> describe_portal(std::string name);
  Task<void> close_prepared(std::string name);
  Task<Value> call_function(u32 function, std::vector<Parameter> parameters = {}, Format format = Format::binary);
  Task<std::vector<Outcome>> batch(std::vector<Command> commands);
  Result<Pipeline> pipeline();
  // Capture the selected backend now, before the returned Task starts.
  Task<void> request_cancel();
  Result<CancelHandle> cancel_handle() const;
  Task<CopyFormat> start_copy(std::string sql);
  Task<void> write_copy(std::span<const std::byte> data);
  Task<void> finish_copy_send();
  Task<std::optional<std::vector<std::byte>>> read_copy();
  Task<ResultSet> end_copy(std::optional<std::string> error = std::nullopt);
  Result<ResultSet> copy_result() const;
  Result<Results> copy_results() const;
  Task<void> start_rows(std::string sql);
  Task<std::optional<Row>> read_row();
  const std::vector<Column> &row_columns() const noexcept;
  Task<ResultSet> open_portal(std::string name, std::string sql, std::vector<Parameter> parameters = {});
  Task<ResultSet> fetch(std::string name, u32 rows = 128);
  Task<void> close_portal(std::string name);
  Task<Notification> wait_notification();
  Task<void> finish();
  Task<void> reset(Options options);
  Task<void> reset(Options options, Diagnostic &diagnostic);
  Task<void> reset(Options options, ConnectionReport &report);

  template <class F>
    requires std::same_as<std::invoke_result_t<F &, const std::vector<Column> &, Row &>, Task<void>>
  Task<void> for_each_row(std::string sql, F handler)
  {
    return each_row(*this, Borrow{*this}, std::move(sql), std::move(handler));
  }

  Result<void> close() noexcept;
  Result<void> cancel() noexcept;
  bool open() const noexcept;
  // Synchronous serialized inspection, including callbacks. Closed/moved sessions are unknown.
  Transaction transaction() const noexcept;
  // Lease/recovery state, not transport health; a failed pipeline keeps its lease until released.
  PipelineStatus pipeline_status() const noexcept;
  u32 backend_process() const noexcept;
  ProtocolVersion protocol_version() const noexcept;
  Authentication authentication_method() const noexcept;
  bool gss_encrypted() const noexcept;
  Result<ConnectionInfo> info() const;
  Result<OptionsInfo> configuration() const;
  Result<Encoding> client_encoding() const noexcept;
  Task<void> set_client_encoding(Encoding encoding);
  Task<std::string> password_verifier(std::string user, std::string_view password, PasswordOptions options = {});
  Task<ResultSet> change_password(std::string user, std::string_view password, PasswordOptions options = {});
  Result<std::optional<std::string>> parameter(std::string_view name) const;
  Result<std::string> escape_literal(std::string_view text) const;
  Result<std::string> escape_identifier(std::string_view text) const;
  Diagnostic last_error() const;
  Result<Failure> last_failure() const;
  Result<NoticeHandler> on_notice(NoticeHandler handler);
  Result<NotificationHandler> on_notification(NotificationHandler handler);
  Result<Trace> on_trace(Trace trace);
  Result<EventId> on_event(std::string name, EventHandler handler);
  Result<void> attach_events(ResultSet &result);
  Result<EventData> event_data(EventId id) const;
  Result<void> set_event_data(EventId id, EventData data);
  std::vector<Diagnostic> take_notices();
  std::vector<Notification> take_notifications();
};

} // namespace weave::pg

template <>
struct std::is_error_code_enum<weave::pg::Error> : std::true_type {};
