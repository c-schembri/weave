#pragma once

#include <weave/tls.hpp>
#include <weave/postgres/row.hpp>
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
struct PoolAccess;
struct MigrationAccess;
struct OwnedOptions;
struct ResultEvents;
struct EventAccess;
struct ResultAccess;

} // namespace detail

/// Client/protocol failures. SQLSTATE errors have a separate category; transport errors retain theirs.
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

/// Convert a PostgreSQL client failure to the standard error-code channel.
std::error_code make_error_code(Error error) noexcept;
/// Encode a PostgreSQL SQLSTATE as an error code; the view is borrowed only during this call.
std::error_code sql_error(std::string_view sqlstate) noexcept;
/// Copy SQLSTATE text from a SQL error code; returns empty for other categories.
std::string sqlstate(std::error_code error);

/// SCRAM channel-binding policy; require forbids authentication without supported binding evidence.
enum class ChannelBinding {
  disable,
  prefer,
  require
};

/// PostgreSQL SSLRequest negotiation or direct TLS startup, subject to server support.
enum class TlsNegotiation {
  postgres,
  direct
};

/// PostgreSQL transport policy. verify_full is the secure default; weaker modes explicitly relax trust.
enum class TlsMode {
  disable,
  allow,
  prefer,
  require,
  verify_ca,
  verify_full
};
/// Last known session transaction state; in_progress is pending protocol work, not an open transaction.
enum class Transaction {
  idle,
  // An open SQL transaction, not an executing command.
  active,
  failed,
  // Pending protocol work; the last ReadyForQuery state is not definitive yet.
  in_progress,
  unknown
};

/// Pipeline lease/recovery state, independent of socket health.
enum class PipelineStatus {
  off,
  on,
  aborted
};

/// Post-startup server role/read-only selection for host failover.
enum class TargetSession {
  any,
  read_write,
  read_only,
  primary,
  standby,
  prefer_standby
};

/// Startup replication mode; raw replication does not implement WAL decoding or acknowledgement policy.
enum class Replication {
  disabled,
  physical,
  database
};

/// Candidate host ordering; does not imply parallel connection attempts.
enum class HostBalance {
  ordered,
  random
};

/// Permitted PostgreSQL protocol negotiation bounds.
enum class ProtocolVersion : u32 {
  v30 = 196608,
  v32 = 196610
};

/// Authentication method negotiated or selected in an explicit policy.
enum class Authentication {
  none,
  password,
  md5,
  scram_sha256,
  gss,
  sspi,
  oauth
};

/// Owned method allow-list, or deny-list when exclude is true; empty uses normal policy.
struct AuthenticationPolicy {
  std::vector<Authentication> methods;
  bool exclude = false;
};

/// Authentication facts; a challenge is not proof that a password was sent or accepted.
struct AuthenticationInfo {
  Authentication method = Authentication::none;
  // Challenge facts, not proof that a password was sent or accepted.
  bool password_requested = false;
  bool password_missing = false;
  bool complete = false;
};

/// Owning negotiated session snapshot, excluding credentials and cancellation secrets.
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

/// Owned host candidate. An address bypasses DNS while name remains available for identity verification.
struct Host {
  std::string name = "localhost";
  u16 port = 5432;
  std::optional<IpAddress> address;
  std::optional<std::string> password;
};

/// Bounds for protocol messages, retained results, queued notifications and authentication/pipeline work.
/// These are logical retained-data limits, not process RSS limits.
struct Limits {
  std::size_t message_bytes = 16 * 1024 * 1024;
  std::size_t result_bytes = 128 * 1024 * 1024;
  std::size_t queued_notifications = 1024;
  u32 scram_iterations = 1000000;
  std::size_t pipeline_commands = 1024;
};

/// Exportable host configuration without its password.
struct HostInfo {
  std::string name;
  u16 port = 5432;
  std::optional<IpAddress> address;
};

/// Exportable TLS policy snapshot, omitting passphrases and secret key-log paths.
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

/// Exportable OAuth configuration without tokens or client secrets.
struct OAuthOptionsInfo {
  std::string issuer;
  std::string client_id;
  std::optional<std::string> scope;
  bool provider = false;
  std::chrono::milliseconds acquisition_timeout{std::chrono::minutes{30}};
};

/// Owning redacted connection configuration; does not reload ambient files or expose credentials.
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

/// Owning 32-byte SCRAM credential; destruction best-effort cleanses this copy, not caller copies.
class ScramKey {
  std::array<std::byte, 32> bytes_{};

public:
  /// Validate/decode a borrowed base64 key into owned storage synchronously.
  static Result<ScramKey> parse(std::string_view base64);
  /// Copy exactly 32 credential bytes; caller remains responsible for its source buffer.
  explicit ScramKey(std::span<const std::byte, 32> bytes) noexcept;
  ScramKey(const ScramKey &) = default;
  ScramKey &operator=(const ScramKey &) = default;
  ScramKey(ScramKey &&other) noexcept;
  ScramKey &operator=(ScramKey &&other) noexcept;
  ~ScramKey();

  /// Borrow sensitive bytes until movement/destruction; avoid logging or retaining unmanaged copies.
  std::span<const std::byte, 32> bytes() const noexcept
  {
    return bytes_;
  }
};

/// Owning connection configuration. APIs taking Options by value copy lvalues or
/// accept transfers with std::move; the source need not outlive the operation.
/// Credential/provider handles may share state; callback-captured references remain caller-owned.
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

  /// Return a redacted owning snapshot without passwords, SCRAM keys or token/client-secret material.
  OptionsInfo info() const;
  /// Explicitly load default configuration from selected sources, then return a redacted snapshot.
  static Result<OptionsInfo> defaults(ConfigSources sources = {});
  /// Parse URI/keyword syntax synchronously, borrowing text only here; no ambient source discovery.
  static Result<Options> parse(std::string_view connection_string);
  /// Parse and explicitly load selected environment/service/password sources into owned options.
  /// Sources are read now, not when a later connect Task starts; compatibility weakening is opt-in.
  static Result<Options> load(std::string_view connection_string = {}, ConfigSources sources = {});
};

/// Amount of server error detail included in bounded formatting.
enum class DiagnosticVerbosity {
  terse,
  standard,
  verbose,
  sqlstate
};

/// Whether formatted diagnostics include server context fields.
enum class DiagnosticContext {
  never,
  errors,
  always
};

/// Formatting policy; optional query is borrowed only during the synchronous format call.
/// Input/output byte limits bound processing and produced text.
struct DiagnosticFormat {
  DiagnosticVerbosity verbosity = DiagnosticVerbosity::terse;
  DiagnosticContext context = DiagnosticContext::never;
  Encoding encoding = Encoding::utf8;
  std::optional<std::string_view> query;
  std::size_t input_bytes = 1024 * 1024;
  std::size_t output_bytes = 16384;
};

/// Owning server diagnostic fields; views returned by accessors borrow this object.
struct Diagnostic {
  std::vector<std::pair<char, std::string>> fields;

  /// Borrow a protocol diagnostic field, or an empty view if absent.
  std::string_view field(char code) const noexcept;
  /// Borrow the primary server message; empty when absent.
  std::string_view message() const noexcept;
  /// Borrow the SQLSTATE field; empty when absent.
  std::string_view sqlstate() const noexcept;
  /// Produce owning bounded text; malformed input or exceeded limits return an error.
  Result<std::string> format(DiagnosticFormat options = {}) const;
};

/// Owning error-code/diagnostic pair, preserving the distinction between SQL and client failures.
struct Failure {
  std::error_code error;
  Diagnostic diagnostic;

  /// Produce bounded owning diagnostic text, including the error-code fallback when needed.
  Result<std::string> format(DiagnosticFormat options = {}) const;
};

/// Startup stage used for connection-attempt timing and failure attribution.
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

/// Owning bounded attempt evidence, excluding passwords and cancellation credentials.
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

/// Caller-owned startup report, mutated only on the executing Context while connect/reset is pending.
/// Keep alive through cancellation drain; inspect concurrently only after the operation completes.
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

  /// Synchronously format completed/serialized report data into bounded owning text.
  Result<std::string> format(DiagnosticFormat options = {}) const;
};

/// Owned noexcept callback; its diagnostic argument is borrowed for the invocation only.
/// Runs inline during protocol processing; do not re-enter conflicting session operations.
using NoticeHandler = std::move_only_function<void(const Diagnostic &) noexcept>;

/// Wire result classification; row_chunk is partial delivery, not final command success.
enum class ResultKind {
  uninitialized,
  empty_query,
  command,
  tuples,
  description,
  row_chunk,
  acknowledgment
};

/// Static readable result-kind label; no allocation and no borrowed result storage.
std::string_view result_kind_name(ResultKind kind) noexcept;

/// Select retained result contents when copying; rows imply columns, scalar metadata is always retained.
struct ResultCopyOptions {
  bool columns = true;
  bool rows = true;
  bool observers = true;
};

/// Supported synchronous result rendering formats; never invokes a pager or external process.
enum class ResultLayout {
  table,
  delimited,
  html
};

/// Bounded render policy. Views/spans are borrowed only for the synchronous format call.
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

/// Owning retained result independent of connection lifetime. Copies may share backing pools.
/// Row/field views still need their backing storage kept alive and unchanged; SQL NULL differs from empty text.
struct ResultSet {
private:
  detail::ResultArena schema_storage_;
  std::shared_ptr<const detail::RowSchema> row_schema_;
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

  /// Copy selected contents; rows imply columns and scalar/command/parameter metadata is always kept.
  ResultSet copy(ResultCopyOptions options = {}) const;
  /// Count retained requests for distinct reachable pools, not RSS or exclusively owned bytes.
  /// Traverses fields synchronously and may allocate temporary bookkeeping.
  std::size_t memory_size() const;
  /// Apply PostgreSQL identifier lookup rules (including quoted identifiers); missing returns nullopt.
  /// Input is borrowed only during this synchronous call; encoding/size failures return errors.
  Result<std::optional<std::size_t>> column_index(
    std::string_view identifier,
    Encoding encoding = Encoding::utf8,
    std::size_t limit = 65536) const;
  /// Bind one exact case-sensitive column name; missing/ambiguous names return decode errors.
  Result<ColumnHandle> column(std::string_view name) const;

  /// Bind a struct's columns once, retaining schema identity but not extending borrowed field-data lifetimes.
  /// The decoder only accepts rows from that schema; decoded string_view members still borrow row storage.
  template <class T>
  Result<RowDecoder<T>> decoder() const;

  /// Decode affected-row count; non-count tags yield zero, malformed numeric counts return an error.
  Result<u64> affected_rows() const noexcept;
  /// Decode legacy inserted OID; zero means no inserted OID.
  Result<u32> inserted_oid() const noexcept;
  /// Borrow legacy OID text from the command tag; keep this result's backing tag alive.
  Result<std::string_view> inserted_oid_text() const noexcept;
  /// Render owning bounded text synchronously; no pager or filesystem side effects.
  Result<std::string> format(ResultFormat options = {}) const;
  /// Look up retained application event data by registration ID.
  Result<EventData> event_data(EventId id) const;
  /// Replace retained application event data; unknown IDs return an error.
  Result<void> set_event_data(EventId id, EventData data);
};

/// Owning wire-ordered results of a simple-query exchange.
using Results = std::vector<ResultSet>;

/// Owning parameter bytes: nullopt is SQL NULL, type zero lets the server infer the OID.
struct Parameter {
  std::optional<std::string> data;
  u32 type = 0;
  Format format = Format::text;
};

/// Non-owning parameter bytes; array and referenced payloads must survive the consuming operation.
struct ParameterView {
  std::optional<std::string_view> data;
  u32 type = 0;
  Format format = Format::text;
};

/// Caller-buffer COPY fragment; end_of_message preserves CopyData boundaries, including empty messages.
struct CopyRead {
  std::size_t size = 0;
  bool end_of_message = false;
};

/// Owning LISTEN/NOTIFY event (backend PID, channel and payload).
struct Notification {
  u32 process = 0;
  std::string channel;
  std::string payload;
};

/// Owned inline noexcept observer; notification is borrowed for this invocation only.
using NotificationHandler = std::move_only_function<void(const Notification &) noexcept>;

/// Owned SQL/parameters queued for batch or pipeline execution; lvalues copy, rvalues transfer.
struct Command {
  std::string sql;
  std::vector<Parameter> parameters;
  Format format = Format::text;
};

/// SQL result or retained diagnostic. SQL failure stays a value; transport/protocol failures remain errors.
/// aborted marks work skipped after a pipeline failure, not successful execution.
struct Outcome {
  std::optional<ResultSet> result;
  Diagnostic error;
  bool aborted = false;

  /// Build a bounded application-created failure value, not a successful wire result.
  [[nodiscard]] static Result<Outcome> failure(Diagnostic diagnostic, std::size_t limit = 1024 * 1024);
};

/// COPY send, receive or duplex mode negotiated by the server.
enum class CopyDirection {
  input,
  output,
  both
};

/// Owning COPY direction and per-column format metadata, not a row/CSV codec.
struct CopyFormat {
  Format format = Format::text;
  std::vector<Format> columns;
  CopyDirection direction = CopyDirection::input;
};

/// Exchange receive-half completion; duplex application sends are not implicitly closed.
struct CopyDone {};

/// Owning wire-ordered query/COPY event. COPY bytes retain native message boundaries.
using ExchangeEvent = std::variant<ResultSet, CopyFormat, std::vector<std::byte>, CopyDone>;

/// Zero buffers rows; positive chunk_rows is an upper bound, not an exact delivery size.
/// Delivered chunks are not final command success or proof of transaction commit.
struct RowOptions {
  u32 chunk_rows = 0;
};

/// Move-only lease for a mixed query/COPY exchange; Connection must outlive it and its method Tasks.
/// Consume to ReadyForQuery, then finish() before reusing the session; abandonment can retire it.
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

  /// Await the next owning wire-order event; nullopt means the exchange reached completion.
  /// One reader only; row chunks are partial results, not final command success.
  Task<std::optional<ExchangeEvent>> next();
  /// Send a COPY message, borrowing data through completion/cancellation; only in a send-capable phase.
  Task<void> write(std::span<const std::byte> data);
  /// Close only the COPY send half; own an optional failure message. Still consume terminal events.
  Task<void> finish_send(std::optional<std::string> error = std::nullopt);
  /// Synchronously release a fully drained exchange lease; pending work/undrained events report busy.
  Result<void> finish();
};

/// Queued protocol operation associated with a PipelineResult ID.
enum class PipelineKind {
  execute,
  prepare,
  describe,
  close,
  sync
};

/// Owning pipeline delivery. complete == false is a row chunk; sync carries transaction state.
struct PipelineResult {
  u64 id = 0;
  PipelineKind kind = PipelineKind::execute;
  Outcome outcome;
  std::optional<Transaction> transaction;
  bool complete = true;
};

/// Static status label for a result; no allocation or retained reference to the argument.
std::string_view status_name(const ResultSet &result) noexcept;
/// Static status label for a server diagnostic.
std::string_view status_name(const Diagnostic &diagnostic) noexcept;
/// Static status label distinguishing SQL result, failure and skipped pipeline work.
std::string_view status_name(const Outcome &outcome) noexcept;
/// Static status label for a COPY direction.
std::string_view status_name(const CopyFormat &format) noexcept;
/// Static status label for the active exchange event alternative.
std::string_view status_name(const ExchangeEvent &event) noexcept;
/// Static status label for a pipeline delivery.
std::string_view status_name(const PipelineResult &result) noexcept;
/// Static status label for an error category/value, not detailed diagnostic text.
std::string_view status_name(std::error_code error) noexcept;

/// Move-only session lease with synchronous owning enqueue and asynchronous send/receive/delivery.
/// Keep Connection and Pipeline alive through deferred method Tasks. Explicit Sync barriers recover
/// SQL abort state; Flush is not Sync. On a stealing Runtime keep send/receive in one when_all graph.
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

  /// Own/copy a command into the bounded queue synchronously and return its delivery ID; does not send.
  Result<u64> execute(Command command, RowOptions rows = {});
  /// Enqueue an owned statement name/parameters; lvalues copy, rvalues transfer.
  Result<u64> execute_prepared(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text,
    RowOptions rows = {});
  /// Enqueue an owned Parse command and return its ID, without performing network I/O.
  Result<u64> prepare(std::string name, std::string sql, std::vector<u32> types = {});
  /// Enqueue statement metadata inspection using an owned name.
  Result<u64> describe(std::string name);
  /// Enqueue portal metadata inspection using an owned name.
  Result<u64> describe_portal(std::string name);
  /// Enqueue a statement close using an owned name.
  Result<u64> close_prepared(std::string name);
  /// Enqueue a portal close using an owned name.
  Result<u64> close_portal(std::string name);
  /// Enqueue a Sync barrier and return its ID; SQL abort recovery occurs when it is processed.
  Result<u64> sync();
  /// Enqueue a protocol Flush without a delivery ID or SQL abort recovery; does not send by itself.
  Result<void> request_flush();

  /// Drive queued sends and receives together; bounded delivery may require a concurrent next() consumer.
  Task<void> flush();
  /// Send queued wire requests without receiving; does not clear outstanding result or Sync debt.
  Task<void> send();
  /// Drive inbound results for submitted work; serialize session access with send() on a stealing Runtime.
  Task<void> receive();
  /// Await one owning delivery; nullopt means no outstanding deliveries remain, not temporary emptiness.
  Task<std::optional<PipelineResult>> next();
  /// Consume an immediately available delivery; pending/empty returns resource_unavailable_try_again.
  /// Returns nullopt when drained; cannot race an active asynchronous delivery consumer.
  Result<std::optional<PipelineResult>> try_next();
  /// Release the lease synchronously only after all requests, results and Sync recovery have drained.
  Result<void> finish();
  /// Whether SQL abort state is still awaiting a processed Sync barrier; not a socket-health test.
  bool aborted() const noexcept;
};

/// Availability probe classification; authentication rejection does not imply no server response.
enum class ServerStatus {
  accepting,
  rejecting,
  no_response
};

/// Probe server availability using configuration owned before the lazy Task starts.
/// Passing options copies it; passing std::move(options) transfers it. No caller Options reference is retained.
Task<ServerStatus> ping(Options options);

class Connection;

/// Establish a connection with its own configuration, captured before the lazy Task starts.
/// connect(options) copies the configuration; connect(std::move(options)) transfers it to avoid that copy.
/// The source Options need not survive the Task or connection; moved-from contents are unspecified.
/// The connection retains configuration for session operations, but may cleanse consumed credentials.
/// Notice callbacks are owned; referenced state inside their captures is not lifetime-extended.
Task<Connection> connect(Options options, NoticeHandler notices = {});

/// Establish a connection, copying lvalue options or transferring std::move(options).
/// The source Options is not borrowed. diagnostic is borrowed from this call through Task completion,
/// including cancellation cleanup; do not destroy or access it concurrently while the Task is pending.
Task<Connection> connect(Options options, Diagnostic &diagnostic, NoticeHandler notices = {});

/// Establish a connection, copying lvalue options or transferring std::move(options).
/// Options and callback objects are owned before the lazy Task starts; callback-captured references are not.
Task<Connection> connect(Options options, NoticeHandler notices, Trace trace);

/// Establish a connection with owned callbacks; lvalue options copy, std::move(options) transfers.
/// diagnostic is borrowed until Task completion, including cancellation cleanup.
/// Callback-captured references remain caller-owned; do not access diagnostic concurrently while pending.
Task<Connection> connect(Options options, Diagnostic &diagnostic, NoticeHandler notices, Trace trace);

/// Establish a connection, copying lvalue options or transferring std::move(options).
/// report is borrowed from this call through Task completion, including cancellation cleanup;
/// keep it alive and do not read or modify it concurrently while the Task is pending.
Task<Connection> connect(Options options, ConnectionReport &report, NoticeHandler notices = {});

/// Establish a connection with owned callbacks; lvalue options copy, std::move(options) transfers.
/// report is borrowed until Task completion, including cancellation cleanup.
/// Callback-captured references remain caller-owned; do not access report concurrently while pending.
Task<Connection> connect(Options options, ConnectionReport &report, NoticeHandler notices, Trace trace);

/// Shared immutable cancellation snapshot independent of Connection lifetime.
/// Requests target the captured backend, not a later reset session; acceptance is not query completion.
class CancelHandle {
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  friend class Connection;

  explicit CancelHandle(std::shared_ptr<const Impl> impl) noexcept;

public:
  /// Request server cancellation on the active Context; returned task owns the snapshot, not this handle.
  Task<void> request() const;
  /// Request server cancellation synchronously on a private driver; does not wait for query completion.
  Result<void> request_blocking() const;
};

/// An owned session. Method Tasks borrow this session even before they start;
/// keep it alive and drain those Tasks before moving or destroying it.
/// By-value strings/containers are owned by operations; view/span inputs are borrowed unless documented as snapshotted.
/// Serialize protocol use on compatible execution; independent runtime roots must not race this session.
/// Dedicated pipeline/COPY APIs provide their documented duplex coordination, not arbitrary concurrent queries.
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
  friend struct detail::PoolAccess;
  friend struct detail::MigrationAccess;
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
  /// Move an idle, unborrowed, non-pooled session; pending method Tasks/leases prohibit movement.
  Connection(Connection &&other) noexcept;
  Connection(const Connection &) = delete;
  /// Destroy after all borrowers/active operations drain; does not asynchronously finish protocol work.
  ~Connection();

  /// Run SQL owned before the lazy Task starts. Lvalue strings copy; std::move(sql) transfers.
  /// Returned results own their data independently of the connection.
  Task<Results> query(std::string sql);

  /// Like query(), with owned SQL and SQL errors returned as outcomes rather than Task failures.
  Task<std::vector<Outcome>> query_outcomes(std::string sql);

  /// Start an exchange owning its SQL; the returned exchange borrows this session until finished.
  Task<Exchange> exchange(std::string sql, RowOptions rows = {});

  /// Own SQL and parameter data before the lazy Task starts; lvalues copy and rvalues transfer.
  /// Returned results own their data. Use execute_borrowed() to avoid copying caller input storage.
  Task<ResultSet> execute(std::string sql, std::vector<Parameter> parameters = {}, Format format = Format::text);

  /// Like execute(), owning SQL/parameters and returning SQL errors as outcomes.
  Task<Outcome> execute_outcome(std::string sql, std::vector<Parameter> parameters = {}, Format format = Format::text);

  /// Own name, SQL and type IDs before the lazy Task starts; lvalues copy and rvalues transfer.
  Task<void> prepare(std::string name, std::string sql, std::vector<u32> types = {});

  /// Borrow SQL, the parameter array and each parameter's data without owning input copies.
  /// Keep that storage alive and unchanged from this call through Task completion, including cancellation cleanup.
  /// Returned results still own their data; borrowed inputs do not make the entire operation allocation-free.
  Task<ResultSet> execute_borrowed(
    std::string_view sql,
    std::span<const ParameterView> parameters = {},
    Format format = Format::text);
  /// Like execute_borrowed(), including input lifetimes, but returns SQL errors as outcomes.
  Task<Outcome> execute_outcome_borrowed(
    std::string_view sql,
    std::span<const ParameterView> parameters = {},
    Format format = Format::text);
  /// Borrow name, SQL and type IDs; keep their storage alive and unchanged
  /// through Task completion, including cancellation cleanup.
  Task<void> prepare_borrowed(std::string_view name, std::string_view sql, std::span<const u32> types = {});
  /// Borrow the statement name, parameter array and parameter data through Task completion/cancellation cleanup.
  /// Returned results own their data; the prepared statement itself belongs to this session.
  Task<ResultSet> execute_prepared_borrowed(
    std::string_view name,
    std::span<const ParameterView> parameters = {},
    Format format = Format::text);
  /// Like execute_prepared_borrowed(), including input lifetimes, but returns SQL errors as outcomes.
  Task<Outcome> execute_prepared_outcome_borrowed(
    std::string_view name,
    std::span<const ParameterView> parameters = {},
    Format format = Format::text);
  /// Own the statement name and parameter data before the lazy Task starts; lvalues copy and rvalues transfer.
  Task<ResultSet> execute_prepared(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  /// Like execute_prepared(), with owned inputs and SQL errors returned as outcomes.
  Task<Outcome> execute_prepared_outcome(
    std::string name,
    std::vector<Parameter> parameters = {},
    Format format = Format::text);
  /// Own a prepared statement name and return its parameter/column description.
  Task<ResultSet> describe(std::string name);
  /// Own a portal name and return its column description.
  Task<ResultSet> describe_portal(std::string name);
  /// Own a name and close its session-bound prepared statement.
  Task<void> close_prepared(std::string name);
  /// Invoke a PostgreSQL function by OID using the fast-path protocol; owns parameters and returned Value.
  Task<Value> call_function(u32 function, std::vector<Parameter> parameters = {}, Format format = Format::binary);
  /// Own all commands and return ordered SQL outcomes; transport/protocol failures fail the task.
  Task<std::vector<Outcome>> batch(std::vector<Command> commands);
  /// Synchronously acquire an exclusive pipeline lease on an idle session; does not perform I/O.
  Result<Pipeline> pipeline();
  /// Snapshot the selected backend before suspension and request server cancellation separately.
  /// This is not local I/O cancellation and does not wait for the target query to finish.
  Task<void> request_cancel();
  /// Synchronously copy a backend cancellation snapshot; it remains independent of later reset/close.
  Result<CancelHandle> cancel_handle() const;
  /// Own SQL and enter a COPY exchange; subsequent COPY calls must complete before session reuse.
  Task<CopyFormat> start_copy(std::string sql);
  /// Send one borrowed CopyData payload; keep storage alive unchanged through completion/cancellation.
  Task<void> write_copy(std::span<const std::byte> data);
  /// Close only the COPY send half; COPY BOTH may still receive and needs end_copy().
  Task<void> finish_copy_send();
  /// Receive one owned CopyData message; nullopt is receive EOF, an empty vector is an empty message.
  Task<std::optional<std::vector<std::byte>>> read_copy();
  /// Receive into nonempty borrowed storage, retained through completion/cancellation.
  /// nullopt is EOF; engaged zero size is an empty message. end_of_message marks the final fragment.
  Task<std::optional<CopyRead>> read_copy(std::span<std::byte> buffer);
  /// Own an optional COPY failure message, finish sends and drain terminal results before session reuse.
  Task<ResultSet> end_copy(std::optional<std::string> error = std::nullopt);
  /// Copy the retained terminal COPY result synchronously, when available.
  Result<ResultSet> copy_result() const;
  /// Copy all retained terminal COPY results in wire order, including replication completion results.
  Result<Results> copy_results() const;
  /// Own SQL and start single-row streaming; consume read_row() to completion before reusing the session.
  Task<void> start_rows(std::string sql);
  /// Return the next retained row or nullopt after ReadyForQuery; SQL/transport failures fail the task.
  Task<std::optional<Row>> read_row();
  /// Borrow the active streaming schema; invalidated by session/schema replacement or destruction.
  const std::vector<Column> &row_columns() const noexcept;
  /// Own name, SQL and parameters to bind a session/transaction-scoped portal; return its description.
  Task<ResultSet> open_portal(std::string name, std::string sql, std::vector<Parameter> parameters = {});
  /// Own a portal name and fetch a bounded batch; suspended indicates more portal rows remain.
  Task<ResultSet> fetch(std::string name, u32 rows = 128);
  /// Own the name and close a session-bound portal.
  Task<void> close_portal(std::string name);
  /// Wait for an owning LISTEN/NOTIFY event; session must not have a competing protocol reader.
  Task<Notification> wait_notification();
  /// Gracefully send Terminate and close the session; unlike close(), may perform network I/O.
  Task<void> finish();
  /// Reconnect using an owned configuration captured before the lazy Task starts.
  /// reset(options) copies; reset(std::move(options)) transfers. The source Options is not borrowed.
  Task<void> reset(Options options);

  /// Reset with owned options (copy lvalues, transfer rvalues); diagnostic is borrowed through
  /// Task completion/cancellation cleanup and must not be accessed concurrently while pending.
  Task<void> reset(Options options, Diagnostic &diagnostic);

  /// Reset with owned options (copy lvalues, transfer rvalues); report is borrowed through
  /// Task completion/cancellation cleanup and must not be accessed concurrently while pending.
  Task<void> reset(Options options, ConnectionReport &report);

  /// Own SQL/callback and await each (columns, Row&) -> Task<void> invocation before reading again.
  /// Callback arguments are borrowed through that invocation only; failure/early exit closes the session.
  template <class F>
    requires std::same_as<std::invoke_result_t<F &, const std::vector<Column> &, Row &>, Task<void>>
  Task<void> for_each_row(std::string sql, F handler)
  {
    return each_row(*this, Borrow{*this}, std::move(sql), std::move(handler));
  }

  /// Abort transport synchronously; active protocol work or observer re-entry can return busy.
  /// Deferred Tasks/leases still borrow this Connection and must drain before destruction.
  Result<void> close() noexcept;
  /// Mark the session closed and request local transport cancellation, not a server CancelRequest.
  /// Pending Tasks must still drain; this does not leave a reusable connected session.
  Result<void> cancel() noexcept;
  /// Whether the session is locally open; not a liveness probe of the peer.
  bool open() const noexcept;
  /// Synchronous serialized inspection, including callbacks; closed/moved sessions report unknown.
  Transaction transaction() const noexcept;
  /// Lease/recovery state, not transport health; a failed pipeline retains its lease until released.
  PipelineStatus pipeline_status() const noexcept;
  /// Selected backend PID, not its cancellation secret.
  u32 backend_process() const noexcept;
  /// Negotiated wire protocol version.
  ProtocolVersion protocol_version() const noexcept;
  /// Authentication method used for the selected session.
  Authentication authentication_method() const noexcept;
  /// Whether transport uses GSS encryption (distinct from GSS authentication).
  bool gss_encrypted() const noexcept;
  /// Return an owning negotiated-session snapshot without secret credentials.
  Result<ConnectionInfo> info() const;
  /// Return an owning redacted configuration snapshot without reloading ambient sources.
  Result<OptionsInfo> configuration() const;
  /// Inspect the tracked client encoding synchronously.
  Result<Encoding> client_encoding() const noexcept;
  /// Set server client_encoding through protocol I/O and update tracked encoding after success.
  Task<void> set_client_encoding(Encoding encoding);
  /// Own the username and copy the password view into secret storage before the lazy Task starts.
  /// Unlike borrowed query inputs, the caller's password buffer need not survive Task execution.
  Task<std::string> password_verifier(std::string user, std::string_view password, PasswordOptions options = {});
  /// Own the username and snapshot the password view before the lazy Task starts; caller buffers are not retained.
  Task<ResultSet> change_password(std::string user, std::string_view password, PasswordOptions options = {});
  /// Copy a tracked ParameterStatus value; nullopt is unknown. Name is borrowed only during this call.
  Result<std::optional<std::string>> parameter(std::string_view name) const;
  /// Quote a literal using tracked session encoding; borrows input only here and owns output.
  Result<std::string> escape_literal(std::string_view text) const;
  /// Quote an identifier using tracked session encoding; borrows input only here and owns output.
  Result<std::string> escape_identifier(std::string_view text) const;
  /// Copy the last retained server diagnostic; client/native errors may have no server fields.
  Diagnostic last_error() const;
  /// Copy the last error-code/diagnostic pair; no previous failure yields a zero-code/empty pair.
  /// Conflicting pending use or a moved-from session returns busy/closed instead.
  Result<Failure> last_failure() const;
  /// Own/replace the inline noexcept notice observer, returning the previous observer.
  Result<NoticeHandler> on_notice(NoticeHandler handler);
  /// Own/replace the inline noexcept notification observer, returning the previous observer.
  Result<NotificationHandler> on_notification(NotificationHandler handler);
  /// Own/replace tracing policy and return the old policy; application payload tracing is sensitive.
  Result<Trace> on_trace(Trace trace);
  /// Register an owned named lifecycle observer; callback references and Event arguments remain borrowed.
  Result<EventId> on_event(std::string name, EventHandler handler);
  /// Attach registered lifecycle observers to an application result synchronously.
  Result<void> attach_events(ResultSet &result);
  /// Copy shared application data for a registered lifecycle observer.
  Result<EventData> event_data(EventId id) const;
  /// Replace application event data synchronously; unknown registrations fail.
  Result<void> set_event_data(EventId id, EventData data);
  /// Move queued owning notices out and clear the queue; no protocol I/O.
  std::vector<Diagnostic> take_notices();
  /// Move queued owning notifications out and clear the queue; does not wait for new messages.
  std::vector<Notification> take_notifications();
};

} // namespace weave::pg

template <>
struct std::is_error_code_enum<weave::pg::Error> : std::true_type {};
