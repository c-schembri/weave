#include <weave/postgres/connection.hpp>
#include "auth.hpp"
#include "large_object.hpp"
#include "options.hpp"
#include "gss_pool.hpp"
#include "gss_stream.hpp"
#include "oauth.hpp"
#include "events.hpp"
#include "result_access.hpp"
#include <weave/timer.hpp>
#include <weave/channel.hpp>
#include <weave/semaphore.hpp>
#include <weave/resolve.hpp>
#include <weave/local.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <variant>
#include <mutex>
#include <limits>
#include <numeric>
#include <random>

namespace weave::pg {

namespace {

using Transport = std::variant<TcpStream, TlsStream<TcpStream>, LocalStream, detail::GssStream>;
enum class CopyMode {
  none,
  input,
  output,
  both,
  rows,
  pipeline,
  exchange
};

enum class ExchangeKind {
  command,
  description,
  portal_description,
  fetch
};

template <bool RetainErrors>
using ExchangeResults = std::conditional_t<RetainErrors, std::vector<Outcome>, Results>;

template <bool RetainErrors>
void append_result(ExchangeResults<RetainErrors> &results, ResultSet &&result)
{
  if constexpr (RetainErrors)
    results.push_back({.result = std::move(result)});
  else
    results.push_back(std::move(result));
}

using PasswordCleanup = detail::OptionsCleanup;

u32 server_version_number(std::string_view text) noexcept
{
  std::array<u32, 3> components{};
  std::size_t count = 0;
  for (auto &component : components) {
    auto start = text.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string_view::npos)
      break;
    text.remove_prefix(start);
    if (text.front() == '-')
      return 0;
    if (text.front() == '+')
      text.remove_prefix(1);

    auto parsed = std::from_chars(text.data(), text.data() + text.size(), component);
    if (parsed.ec == std::errc::result_out_of_range)
      return 0;
    if (parsed.ec != std::errc{})
      break;

    ++count;
    text.remove_prefix(static_cast<std::size_t>(parsed.ptr - text.data()));
    if (text.empty() || text.front() != '.')
      break;
    text.remove_prefix(1);
  }
  if (!count)
    return 0;

  u64 number = static_cast<u64>(components[0]) * 10000;
  if (count == 3)
    number += static_cast<u64>(components[1]) * 100 + components[2];
  else if (count == 2)
    number += static_cast<u64>(components[1]) * (components[0] >= 10 ? 1 : 100);
  if (number > static_cast<u64>(std::numeric_limits<i32>::max()))
    return 0;
  return static_cast<u32>(number);
}

class ConnectionHistory {
  ConnectionReport &report_;
  ConnectionAttempt current_;
  std::size_t bytes_ = 0;
  std::optional<std::size_t> recorded_;
  bool finished_ = false;
  bool oversized_ = false;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point stage_started_ = started_;

  void update() noexcept
  {
    const auto now = std::chrono::steady_clock::now();
    auto &elapsed = current_.stage_times[static_cast<std::size_t>(current_.stage)];
    elapsed += std::chrono::duration_cast<std::chrono::microseconds>(now - stage_started_);
    current_.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - started_);
    stage_started_ = now;
  }

public:
  explicit ConnectionHistory(ConnectionReport &report) : report_(report)
  {
  }

  void begin(std::string_view host, u16 port, std::optional<Endpoint> endpoint = {})
  {
    update();
    const auto resolution = current_.stage == ConnectionStage::resolution && !finished_
      ? current_.stage_times[static_cast<std::size_t>(ConnectionStage::resolution)]
      : std::chrono::microseconds{};
    current_ = {};
    started_ = std::chrono::steady_clock::now() - resolution;
    stage_started_ = std::chrono::steady_clock::now();
    current_.stage_times[static_cast<std::size_t>(ConnectionStage::resolution)] = resolution;
    report_.authentication = {};
    current_.port = port;
    current_.endpoint = endpoint;
    oversized_ = host.size() > ConnectionReport::max_bytes;
    if (!oversized_)
      current_.host = host;
    recorded_.reset();
    finished_ = false;
    report_.current = current_;
  }

  void stage(ConnectionStage stage) noexcept
  {
    update();
    current_.stage = stage;
    report_.current = current_;
  }

  void authentication(AuthenticationInfo info) noexcept
  {
    current_.authentication = info;
    report_.authentication = info;
    report_.current.authentication = info;
  }

  void endpoint(Endpoint endpoint) noexcept
  {
    current_.endpoint = endpoint;
    report_.current.endpoint = endpoint;
  }

  void complete()
  {
    update();
    report_.current = current_;
  }

  void fail(std::error_code error, const Diagnostic *diagnostic = nullptr)
  {
    // The timeout owner reports timed_out after its cancelled child has drained.
    // Keep one entry for that attempt, using the owner's final error rather than a duplicate.
    if (finished_) {
      if (current_.error == std::errc::operation_canceled && error == std::errc::timed_out) {
        if (recorded_)
          report_.attempts[*recorded_].error = error;
        report_.current.error = error;
      }
      return;
    }
    finished_ = true;
    update();
    current_.error = error;
    report_.current = current_;

    auto bytes = sizeof(ConnectionAttempt) + current_.host.size();
    if (oversized_ || report_.attempts.size() == ConnectionReport::max_attempts ||
      bytes > ConnectionReport::max_bytes - bytes_) {
      report_.truncated = true;
      return;
    }

    auto diagnostic_bytes = bytes;
    bool oversized = false;
    if (diagnostic) {
      oversized = diagnostic->fields.size() > 255;
      for (const auto &[code, value] : diagnostic->fields) {
        auto overhead = sizeof(std::pair<char, std::string>);
        if (value.size() > ConnectionReport::max_bytes ||
          diagnostic_bytes > ConnectionReport::max_bytes - value.size() ||
          overhead > ConnectionReport::max_bytes - diagnostic_bytes - value.size()) {
          oversized = true;
          break;
        }
        diagnostic_bytes += overhead + value.size();
      }
    }
    if (oversized || diagnostic_bytes > ConnectionReport::max_bytes - bytes_) {
      current_.diagnostic_truncated = true;
      report_.truncated = true;
    } else if (diagnostic) {
      current_.diagnostic = *diagnostic;
      bytes = diagnostic_bytes;
    }

    bytes_ += bytes;
    recorded_ = report_.attempts.size();
    report_.current = current_;
    report_.attempts.push_back(current_);
  }
};

void finish_report(ConnectionReport &report, std::error_code error, const Diagnostic &diagnostic)
{
  report.error = error;
  report.completed = true;
  if (error && report.attempts.empty() && !report.truncated) {
    ConnectionHistory history{report};
    history.begin({}, 0);
    history.fail(error, &diagnostic);
  }
}

bool valid_trace(const Trace &trace) noexcept
{
  return (trace.content == TraceContent::metadata || trace.content == TraceContent::application) &&
    trace.payload_bytes <= 16 * 1024 * 1024;
}

struct OAuthAttempt {
  std::optional<OAuthToken> token;
  std::optional<detail::OAuthDiscovery> discovery;
};

struct ConnectProgress {
  bool transport_connected = false;
  bool direct_tls_pending = false;
  bool credentials_failed = false;
  bool tls_handshake_failed = false;
};

TlsMode tls_mode(const Options &options) noexcept
{
  return options.plaintext ? TlsMode::disable : options.tls_mode;
}

bool valid_protocol(ProtocolVersion version)
{
  return version == ProtocolVersion::v30 || version == ProtocolVersion::v32;
}

bool allows_authentication(const AuthenticationPolicy &policy, Authentication method)
{
  if (policy.methods.empty())
    return true;
  bool listed = std::ranges::find(policy.methods, method) != policy.methods.end();
  return listed != policy.exclude;
}

bool password_requested(Authentication method, std::span<const std::byte> payload) noexcept
{
  if (method == Authentication::password)
    return payload.empty();
  if (method == Authentication::md5)
    return payload.size() == 4;
  if (method != Authentication::scram_sha256)
    return false;

  bool offered = false;
  while (!payload.empty()) {
    auto terminator = std::ranges::find(payload, std::byte{});
    if (terminator == payload.end())
      return false;
    auto size = static_cast<std::size_t>(terminator - payload.begin());
    if (!size)
      return payload.size() == 1 && offered;

    std::string_view name{reinterpret_cast<const char *>(payload.data()), size};
    offered |= name == "SCRAM-SHA-256" || name == "SCRAM-SHA-256-PLUS";
    payload = payload.subspan(size + 1);
  }
  return false;
}

Result<void> probe_authentication(u32 method, detail::Reader &reader)
{
  if (method == 0 || method == 3 || method == 7 || method == 9) {
    if (reader.empty())
      return {};
  } else if (method == 5) {
    if (reader.rest().size() == 4)
      return {};
  } else if (method == 10) {
    bool advertised = false;
    for (;;) {
      auto mechanism = reader.string();
      if (!reader.valid())
        break;
      if (mechanism.empty()) {
        if (advertised && reader.empty())
          return {};
        break;
      }
      const auto valid_character = [](unsigned char value) {
        return (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') || value == '-' || value == '_';
      };
      if (mechanism.size() > 20 || !std::ranges::all_of(mechanism, valid_character))
        break;
      advertised = true;
    }
  } else if (method != 8 && method != 11 && method != 12) {
    return std::unexpected(make_error_code(Error::unsupported_authentication));
  }

  return std::unexpected(make_error_code(Error::protocol));
}

Result<void> valid_transport(const Options &options, std::string_view host, u16 port, bool numeric_address)
{
  if (!detail::local_host(host)) {
    if (options.required_peer_user)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    return {};
  }
  if (numeric_address)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (auto path = detail::local_socket_path(host, port); !path)
    return std::unexpected(path.error());
  if (!options.plaintext && options.tls_mode != TlsMode::disable)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  if (options.gss_encryption != GssEncryption::disable)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  if (options.oauth)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  bool tcp_controls = options.keep_alive.idle.count() != 0 || options.keep_alive.interval.count() != 0 ||
    options.keep_alive.probes != 0 || options.tcp_user_timeout.count() != 0;
  if (tcp_controls)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  return {};
}

Result<void> valid_options(const Options &options)
{
  constexpr auto maximum_socket_option = std::numeric_limits<int>::max();
  bool invalid_socket_options = options.keep_alive.idle.count() < 0 ||
    options.keep_alive.idle.count() > maximum_socket_option || options.keep_alive.interval.count() < 0 ||
    options.keep_alive.interval.count() > maximum_socket_option ||
    options.keep_alive.probes > static_cast<u32>(maximum_socket_option) || options.tcp_user_timeout.count() < 0 ||
    options.tcp_user_timeout.count() > maximum_socket_option;
  bool invalid_host = options.hosts.empty() &&
    (options.host.empty() || options.host.size() > 65536 || !detail::cstring_valid(options.host) || !options.port);
  bool invalid_identity = options.user.empty() || !detail::cstring_valid(options.user) ||
    !detail::cstring_valid(options.database) || !detail::cstring_valid(options.password) ||
    !detail::cstring_valid(options.application_name) || !detail::cstring_valid(options.server_options) ||
    options.client_encoding.empty() || !detail::cstring_valid(options.client_encoding);
  bool oversized_identity = options.user.size() > 65536 || options.database.size() > 65536 ||
    options.application_name.size() > 65536 || options.server_options.size() > 65536 ||
    options.client_encoding.size() > 65536;
  bool invalid_limits = options.limits.message_bytes < 1024 || options.limits.message_bytes > 1024 * 1024 * 1024 ||
    options.limits.result_bytes < options.limits.message_bytes || options.limits.queued_notifications == 0 ||
    options.limits.scram_iterations < 4096 || options.limits.scram_iterations > 10000000 ||
    options.limits.pipeline_commands == 0 || options.limits.pipeline_commands > 65535;
  bool invalid_policy = (options.plaintext && (options.tls.has_value() || options.tls_options.has_value())) ||
    (tls_mode(options) == TlsMode::disable &&
      (options.tls || options.tls_options || options.channel_binding == ChannelBinding::require)) ||
    options.tls_mode < TlsMode::disable || options.tls_mode > TlsMode::verify_full ||
    (options.plaintext && options.tls_mode != TlsMode::disable && options.tls_mode != TlsMode::verify_full) ||
    (options.tls_negotiation == TlsNegotiation::direct && tls_mode(options) < TlsMode::require) ||
    (options.plaintext && options.tls_negotiation == TlsNegotiation::direct) ||
    options.tls_negotiation < TlsNegotiation::postgres || options.tls_negotiation > TlsNegotiation::direct ||
    options.client_certificate < TlsCertificateMode::disable ||
    options.client_certificate > TlsCertificateMode::require ||
    (options.tls.has_value() && options.tls_options.has_value()) ||
    (options.plaintext && options.channel_binding == ChannelBinding::require) ||
    options.gss_encryption < GssEncryption::disable || options.gss_encryption > GssEncryption::require ||
    options.target_session < TargetSession::any || options.target_session > TargetSession::prefer_standby ||
    options.replication < Replication::disabled || options.replication > Replication::database ||
    options.host_balance < HostBalance::ordered || options.host_balance > HostBalance::random ||
    !valid_protocol(options.min_protocol) || !valid_protocol(options.max_protocol) ||
    options.min_protocol > options.max_protocol ||
    (options.replication == Replication::physical && options.target_session != TargetSession::any);
  if (invalid_host || invalid_identity || oversized_identity || invalid_limits || invalid_policy ||
    invalid_socket_options || options.hosts.size() > 64 || options.password.size() > 65536 ||
    options.settings.size() > 64 || options.connect_timeout <= std::chrono::milliseconds{0} ||
    options.connect_timeout > std::chrono::hours{24})
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  if (auto policy = detail::valid_authentication_policy(options.authentication); !policy)
    return policy;
  if (options.oauth) {
    if (auto valid = detail::valid_oauth_options(*options.oauth); !valid)
      return valid;
    if (!options.oauth->provider)
      return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    if (options.plaintext && options.gss_encryption == GssEncryption::disable)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }
  if (!options.authentication.exclude && !options.oauth &&
    std::ranges::find(options.authentication.methods, Authentication::oauth) != options.authentication.methods.end())
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  if (!detail::valid_gss_target("localhost", options.gss_service) || (options.plaintext && !options.gss_mutual))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (!options.gss && (options.gss_delegation || !options.gss_mutual || options.gss_service != "postgres"))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (options.gss_encryption != GssEncryption::disable && !options.gss)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  if (!options.authentication.exclude && !options.gss) {
    const auto native = [](Authentication method) {
      return method == Authentication::gss || method == Authentication::sspi;
    };
    if (std::ranges::any_of(options.authentication.methods, native))
      return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  }

#if defined(_WIN32)
  if (options.tcp_user_timeout.count() != 0 || options.required_peer_user)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#else
  if (options.required_peer_user && *options.required_peer_user > std::numeric_limits<u32>::max())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
#endif

  if (options.hosts.empty()) {
    if (auto valid = valid_transport(options, options.host, options.port, false); !valid)
      return valid;
  }

  for (const auto &host : options.hosts) {
    bool invalid_password = host.password && (host.password->size() > 65536 || !detail::cstring_valid(*host.password));
    if (host.name.empty() || host.name.size() > 65536 || !detail::cstring_valid(host.name) || !host.port ||
      invalid_password)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (auto valid = valid_transport(options, host.name, host.port, host.address.has_value()); !valid)
      return valid;
  }

  const std::array reserved{"user", "database", "application_name", "client_encoding", "options", "replication"};
  for (const auto &[name, value] : options.settings) {
    bool reserved_name = std::ranges::find(reserved, name) != reserved.end() || name.starts_with("_pq_.");
    if (name.empty() || name.size() > 65536 || value.size() > 65536 || !detail::cstring_valid(name) ||
      !detail::cstring_valid(value) || reserved_name)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  }

  return {};
}

Result<std::unique_ptr<std::mt19937>> host_randomizer(HostBalance policy)
{
  if (policy == HostBalance::ordered)
    return std::unique_ptr<std::mt19937>{};

  std::array<u32, 8> entropy;
  if (RAND_bytes(reinterpret_cast<unsigned char *>(entropy.data()), static_cast<int>(sizeof(entropy))) != 1)
    return std::unexpected(std::make_error_code(std::errc::io_error));

  std::seed_seq seed(entropy.begin(), entropy.end());
  return std::make_unique<std::mt19937>(seed);
}

Result<void> configure_socket(TcpStream &socket, const Options &options)
{
  if (auto result = socket.no_delay(); !result)
    return result;
  if (auto result = socket.keep_alive(options.keep_alive); !result)
    return result;
  if (options.tcp_user_timeout.count() != 0)
    return socket.user_timeout(options.tcp_user_timeout);

  return {};
}

Task<TcpStream> connect_balanced(std::string host, u16 port, std::mt19937 &random)
{
  auto endpoints = co_await resolve(std::move(host), port);
  std::shuffle(endpoints.begin(), endpoints.end(), random);
  co_return co_await tcp::connect(std::move(endpoints));
}

Task<TcpStream> connect_tcp(const Options &options, std::optional<IpAddress> address, std::mt19937 *random)
{
  if (address)
    return tcp::connect(Endpoint{*address, options.port});
  if (random)
    return connect_balanced(options.host, options.port, *random);
  return tcp::connect(options.host, options.port);
}

Task<TcpStream> connect_tcp_reported(
  const Options &options,
  std::optional<IpAddress> address,
  std::mt19937 *random,
  ConnectionHistory &history)
{
  history.stage(ConnectionStage::resolution);
  std::vector<Endpoint> endpoints;
  auto numeric = Endpoint::parse(options.host, options.port);
  if (address)
    endpoints.push_back({*address, options.port});
  else if (numeric && !random)
    endpoints.push_back(*numeric);
  else
    endpoints = co_await resolve(options.host, options.port);
  if (random)
    std::shuffle(endpoints.begin(), endpoints.end(), *random);
  if (endpoints.empty())
    co_await fail(std::errc::invalid_argument);

  std::error_code failure;
  for (auto endpoint : endpoints) {
    history.begin(options.host, options.port, endpoint);
    history.stage(ConnectionStage::transport);
    co_await cancellation_point();
    auto stream = co_await as_result(tcp::connect(endpoint));
    if (stream)
      co_return std::move(*stream);

    failure = stream.error();
    history.fail(failure);
    if (failure == std::errc::operation_canceled)
      co_await fail(failure);
  }
  co_await fail(failure);
}

Task<Transport> connect_transport(
  const Options &options,
  std::optional<IpAddress> address,
  std::mt19937 *random,
  ConnectionHistory *history = nullptr)
{
  if (detail::local_host(options.host)) {
    if (history)
      history->stage(ConnectionStage::transport);
    auto path = detail::local_socket_path(options.host, options.port);
    if (!path)
      co_await fail(path.error());
    auto stream = co_await local::connect(std::move(*path));
    co_return Transport{std::move(stream)};
  }
  auto connecting = history ? connect_tcp_reported(options, address, random, *history)
                            : connect_tcp(options, address, random);
  auto stream = co_await std::move(connecting);
  co_return Transport{std::move(stream)};
}

Result<detail::Writer> parse(std::string_view name, std::string_view sql, const std::vector<u32> &types)
{
  if (!detail::cstring_valid(name) || !detail::cstring_valid(sql) || types.size() > 65535)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  detail::Writer body;
  body.string(name);
  body.string(sql);
  body.integer(static_cast<u32>(types.size()), 2);
  for (auto type : types)
    body.integer(type);

  detail::Writer request;
  request.message('P', body);
  return request;
}

void execution(detail::Writer &request)
{
  detail::Writer describe;
  describe.integer('P', 1);
  describe.string("");
  request.message('D', describe);

  detail::Writer execute;
  execute.string("");
  execute.integer(0);
  request.message('E', execute);
  request.message('S');
}

enum class PipelineStep {
  parse,
  bind,
  parameters,
  description,
  rows,
  close,
  sync
};

struct PipelineEntry {
  u64 id;
  PipelineKind kind;
  PipelineStep step;
  u32 chunk_rows = 0;
};

struct PipelineState;

struct PipelineDelivery {
  PipelineState *owner = nullptr;
  PipelineResult result;
  std::size_t retained = 0;

  PipelineDelivery() = default;
  PipelineDelivery(PipelineDelivery &&other) noexcept;
  PipelineDelivery &operator=(PipelineDelivery &&other) noexcept;
  PipelineDelivery(const PipelineDelivery &) = delete;
  ~PipelineDelivery();
};

struct PipelineState {
  mutable std::mutex mutex;
  detail::ConnectionEvents *events = nullptr;
  Limits limits;
  detail::Writer request;
  std::vector<PipelineEntry> pending;
  detail::Writer outbound;
  std::vector<PipelineEntry> submitted;
  Channel<PipelineDelivery> results;
  Channel<std::monostate> space{1};
  std::atomic<std::size_t> borrowers{0};
  std::size_t outstanding = 0;
  std::size_t retained = 0;
  u64 sequence = 0;
  u64 submitted_sequence = 0;
  bool flushing = false;
  bool sending = false;
  bool reading = false;
  bool receiving = false;
  bool unsynchronized = false;
  bool aborted = false;
  bool terminal = false;
  bool detached = false;
  std::error_code failure;

  explicit PipelineState(Limits configuration) : limits(configuration), results(configuration.pipeline_commands)
  {
  }

  ~PipelineState()
  {
    // Deliveries release through space; drain them while both channels are alive.
    results.close();
    for (;;) {
      auto delivery = results.try_receive();
      if (!delivery || !*delivery)
        break;
    }
    weave::detail::require(retained == 0);
  }

  void stop(std::error_code error) noexcept
  {
    std::lock_guard lock(mutex);
    if (!failure && !detached)
      failure = error;

    terminal = true;
    results.close();
    space.close();
  }

  Result<u64> enqueue(detail::Writer message, PipelineKind kind, PipelineStep step, RowOptions rows = {})
  {
    std::lock_guard lock(mutex);
    if (terminal)
      return std::unexpected(failure ? failure : make_error_code(Error::closed));
    if (outstanding >= limits.pipeline_commands || sequence == UINT64_MAX ||
      message.bytes.size() > limits.message_bytes || message.bytes.size() > limits.result_bytes - request.bytes.size())
      return std::unexpected(make_error_code(Error::resource_limit));

    const auto id = ++sequence;
    request.raw(message.bytes);
    pending.push_back({id, kind, step, rows.chunk_rows});
    ++outstanding;
    return id;
  }

  Result<void> request_flush()
  {
    std::lock_guard lock(mutex);
    if (terminal)
      return std::unexpected(failure ? failure : make_error_code(Error::closed));

    constexpr std::size_t message_size = 5;
    if (message_size > limits.message_bytes || request.bytes.size() > limits.result_bytes ||
      message_size > limits.result_bytes - request.bytes.size())
      return std::unexpected(make_error_code(Error::resource_limit));

    request.message('H');
    return {};
  }

  Task<void> reserve(std::size_t bytes, std::size_t &entry_bytes, std::size_t staging, bool streaming)
  {
    // Waiting cannot reclaim the reader's own staging storage.
    if (staging > limits.result_bytes || bytes > limits.result_bytes - staging)
      co_await fail(Error::resource_limit);

    for (;;) {
      std::error_code error;
      bool available = false;
      {
        std::lock_guard lock(mutex);
        if (terminal)
          error = failure ? failure : make_error_code(Error::closed);
        else if (bytes <= limits.result_bytes - retained) {
          retained += bytes;
          entry_bytes += bytes;
          available = true;
        }
      }
      if (error)
        co_await fail(error);
      if (available)
        co_return;
      if (!streaming)
        co_await fail(Error::resource_limit);

      auto released = co_await space.receive();
      if (!released)
        co_await fail(Error::closed);
    }
  }

  void release(std::size_t bytes)
  {
    {
      std::lock_guard lock(mutex);
      weave::detail::require(bytes <= retained);
      retained -= bytes;
    }
    std::monostate notification;
    static_cast<void>(space.try_send(notification));
  }

  Task<void> publish(PipelineDelivery delivery, bool streaming)
  {
    if (events && delivery.result.outcome.result)
      events->create(*delivery.result.outcome.result);

    auto sent = results.try_send(delivery);
    if (sent)
      co_return;
    if (!streaming || sent.error() != std::errc::resource_unavailable_try_again)
      co_await fail(sent.error());

    co_await results.send(std::move(delivery));
  }

  Result<PipelineResult> consume(PipelineDelivery delivery)
  {
    {
      std::lock_guard lock(mutex);
      weave::detail::require(outstanding != 0);
      if (delivery.result.complete)
        --outstanding;
    }
    return std::move(delivery.result);
  }
};

PipelineDelivery::PipelineDelivery(PipelineDelivery &&other) noexcept
    : owner(std::exchange(other.owner, nullptr)), result(std::move(other.result)),
      retained(std::exchange(other.retained, 0))
{
}

PipelineDelivery &PipelineDelivery::operator=(PipelineDelivery &&other) noexcept
{
  if (this != &other) {
    if (owner)
      owner->release(retained);
    owner = std::exchange(other.owner, nullptr);
    result = std::move(other.result);
    retained = std::exchange(other.retained, 0);
  }
  return *this;
}

PipelineDelivery::~PipelineDelivery()
{
  if (owner)
    owner->release(retained);
}

struct PipelineBuffer {
  PipelineState &state;
  const PipelineEntry &entry;
  bool streaming;
  PipelineDelivery delivery;
  std::vector<Column> schema;
  std::size_t schema_bytes = 0;

  ~PipelineBuffer()
  {
    state.release(schema_bytes);
  }

  Task<void> initialize(bool with_schema = true)
  {
    delivery = {};
    delivery.owner = &state;
    delivery.result.id = entry.id;
    delivery.result.kind = entry.kind;
    auto size = sizeof(PipelineDelivery) + (with_schema ? schema_bytes : 0);
    co_await state.reserve(size, delivery.retained, schema_bytes, streaming);

    if (entry.kind != PipelineKind::sync) {
      delivery.result.outcome.result.emplace();
      delivery.result.outcome.result->kind = ResultKind::acknowledgment;
      if (with_schema)
        delivery.result.outcome.result->columns = schema;
    }
  }

  Task<void> emit()
  {
    delivery.result.complete = false;
    delivery.result.outcome.result->kind = ResultKind::row_chunk;
    auto outgoing = std::move(delivery);
    co_await state.publish(std::move(outgoing), streaming);
    co_await initialize();
  }

  Task<void> reserve(std::size_t bytes)
  {
    auto staged = delivery.retained + schema_bytes;
    bool oversized = staged > state.limits.result_bytes || bytes > state.limits.result_bytes - staged;
    auto &result = delivery.result.outcome.result;
    if (entry.chunk_rows && oversized && result && !result->rows.empty())
      co_await emit();

    co_await state.reserve(bytes, delivery.retained, delivery.retained + schema_bytes, streaming);
  }

  Task<void> publish()
  {
    state.release(std::exchange(schema_bytes, 0));
    schema.clear();
    auto outgoing = std::move(delivery);
    co_await state.publish(std::move(outgoing), streaming);
  }
};

Task<void> cancel_request(Result<CancelHandle> cancellation)
{
  if (!cancellation)
    co_await fail(cancellation.error());

  co_await cancellation->request();
}

Result<CopyFormat> copy_format(const detail::Message &message)
{
  detail::Reader reader{message.body};
  CopyFormat format;
  format.format = static_cast<Format>(reader.integer(1));
  auto count = reader.integer(2);
  for (u32 index = 0; index < count && reader.valid(); ++index) {
    auto column = static_cast<Format>(reader.integer(2));
    if (column != Format::text && column != Format::binary)
      return std::unexpected(make_error_code(Error::protocol));
    format.columns.push_back(column);
  }
  if (!reader.empty() || (format.format != Format::text && format.format != Format::binary))
    return std::unexpected(make_error_code(Error::protocol));

  if (message.kind == 'G')
    format.direction = CopyDirection::input;
  else if (message.kind == 'H')
    format.direction = CopyDirection::output;
  else if (message.kind == 'W')
    format.direction = CopyDirection::both;
  else
    return std::unexpected(make_error_code(Error::protocol));
  return format;
}

bool replication_keepalive(const detail::Message &message)
{
  return message.kind == 'd' && message.body.size() == 18 && message.body.front() == std::byte{'k'} &&
    std::to_integer<unsigned>(message.body.back()) <= 1;
}

} // namespace

struct CancelHandle::Impl {
  Endpoint endpoint;
  std::string local_address;
  std::optional<u64> peer_user;
  std::string host;
  std::optional<TlsContext> tls;
  TlsNegotiation tls_negotiation = TlsNegotiation::postgres;
  bool server_name_indication = true;
  TlsCertificateMode client_certificate = TlsCertificateMode::allow;
  std::optional<GssContext> gss;
  std::string gss_service;
  bool gss_delegation = false;
  bool gss_encrypted = false;
  bool plaintext = false;
  std::chrono::milliseconds deadline{30000};
  u32 process = 0;
  detail::Bytes key;

  ~Impl()
  {
    OPENSSL_cleanse(key.data(), key.size());
  }

  static Task<void> send_gss(detail::GssStream &socket, std::span<const std::byte> request)
  {
    co_await socket.write_all(request);
    if (auto result = socket.shutdown_send(); !result)
      co_await fail(result.error());
    std::array<std::byte, 1> response;
    if (co_await socket.read(response))
      co_await fail(Error::protocol);
  }

  static Task<void> send(std::shared_ptr<const Impl> state)
  {
    detail::Writer request;
    request.integer(12 + static_cast<u32>(state->key.size()));
    request.integer(80877102);
    request.integer(state->process);
    request.raw(state->key);

    struct Cleanup {
      detail::Writer &request;

      ~Cleanup()
      {
        OPENSSL_cleanse(request.bytes.data(), request.bytes.size());
      }
    } cleanup{request};

    if (!state->local_address.empty()) {
      auto socket = co_await local::connect(state->local_address);
      if (state->peer_user) {
        auto peer = socket.peer_credentials();
        if (!peer)
          co_await fail(peer.error());
        if (peer->user != *state->peer_user)
          co_await fail(Error::authentication);
      }
      co_await socket.write_all(request.bytes);
      if (auto result = socket.shutdown_send(); !result)
        co_await fail(result.error());
      std::array<std::byte, 1> response;
      if (co_await socket.read(response))
        co_await fail(Error::protocol);
      co_return;
    }

    auto socket = co_await tcp::connect(state->endpoint);
    if (state->gss_encrypted) {
      weave::detail::require(state->gss.has_value());
      detail::GssOptions policy;
      policy.service = state->gss_service;
      policy.delegate = state->gss_delegation;
      Diagnostic diagnostic;
      auto transport = co_await detail::gss_client(
        std::move(socket),
        *state->gss,
        state->host,
        std::move(policy),
        GssEncryption::require,
        diagnostic);
      auto &secured = std::get<detail::GssStream>(transport);
      auto result = co_await as_result(send_gss(secured, request.bytes));
      co_await secured.finish();
      if (!result)
        co_await fail(result.error());
      co_return;
    }
    if (state->plaintext) {
      co_await socket.write_all(request.bytes);
      auto result = socket.shutdown_send();
#if defined(_WIN32)
      // CancelRequest has no acknowledgement; PostgreSQL may reset after the full send.
      if (!result && result.error() == std::errc::connection_reset)
        co_return;
#endif
      if (!result)
        co_await fail(result.error());

      std::array<std::byte, 1> response;
      auto ended = co_await as_result(socket.read(response));
#if defined(_WIN32)
      if (!ended && ended.error() == std::errc::connection_reset)
        co_return;
#endif
      if (!ended)
        co_await fail(ended.error());
      if (*ended)
        co_await fail(Error::protocol);
    } else {
      if (!state->tls)
        co_await fail(Error::authentication);
      if (state->tls_negotiation == TlsNegotiation::postgres) {
        detail::Writer ssl;
        ssl.integer(8);
        ssl.integer(80877103);
        co_await socket.write_all(ssl.bytes);
        std::array<std::byte, 1> response;
        co_await socket.read_exactly(response);
        if (response[0] != std::byte{'S'})
          co_await fail(Error::authentication);
      }
      TlsHandshakeOptions handshake;
      handshake.timeout = state->deadline;
      handshake.server_name_indication = state->server_name_indication;
      handshake.client_certificate = state->client_certificate;
      if (state->tls_negotiation == TlsNegotiation::direct)
        handshake.required_protocol = "postgresql";

      auto secured = co_await tls::client(std::move(socket), *state->tls, state->host, std::move(handshake));
      co_await secured.write_all(request.bytes);
      auto shutdown = co_await as_result(secured.shutdown_send());
#if defined(_WIN32)
      // After the full request is sent, PostgreSQL may close before our TLS close_notify.
      if (!shutdown && shutdown.error() == std::errc::connection_reset)
        co_return;
#endif
      if (!shutdown)
        co_await fail(shutdown.error());
      std::array<std::byte, 1> response;
      // CancelRequest has no acknowledgement; PostgreSQL may omit close_notify here.
      auto ended = co_await as_result(secured.read(response));
      bool expected_eof = !ended && ended.error() == make_error_code(TlsError::truncated);
#if defined(_WIN32)
      // Windows can report the server's CancelRequest closure as reset instead of EOF.
      expected_eof = expected_eof || (!ended && ended.error() == std::errc::connection_reset);
#endif
      if ((!ended && !expected_eof) || (ended && *ended != 0))
        co_await fail(ended ? make_error_code(Error::protocol) : ended.error());
    }
  }
};

CancelHandle::CancelHandle(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl))
{
}

Task<void> CancelHandle::request() const
{
  weave::detail::require(impl_ != nullptr);
  return timeout(impl_->deadline, Impl::send(impl_));
}

Result<void> CancelHandle::request_blocking() const
{
  weave::detail::require(impl_ != nullptr && !weave::detail::current_context);
  auto ctx = Context::create();
  if (!ctx)
    return std::unexpected(ctx.error());

  return ctx->run(request());
}

struct Connection::NoticeReceiver {
  NoticeHandler handler;
  bool invoking = false;

  explicit NoticeReceiver(NoticeHandler callback) : handler(std::move(callback))
  {
  }

  void deliver(const Diagnostic &notice) noexcept
  {
    weave::detail::require(!invoking);
    invoking = true;
    handler(notice);
    invoking = false;
  }
};

struct Connection::NotificationReceiver {
  NotificationHandler handler;
  bool invoking = false;

  explicit NotificationReceiver(NotificationHandler callback) : handler(std::move(callback))
  {
  }

  void deliver(const Notification &notification) noexcept
  {
    weave::detail::require(!invoking);
    invoking = true;
    handler(notification);
    invoking = false;
  }
};

struct Connection::TraceReceiver {
  Trace configuration;
  std::mutex mutex;
  std::atomic<bool> invoking{false};

  explicit TraceReceiver(Trace trace) : configuration(std::move(trace))
  {
  }

  void deliver(TraceDirection direction, char kind, u32 length, std::span<const std::byte> body, bool ready)
  {
    if (!configuration.handler)
      return;

    static thread_local TraceReceiver *active = nullptr;
    weave::detail::require(active != this);
    std::lock_guard lock{mutex};
    auto *previous = std::exchange(active, this);
    invoking.store(true, std::memory_order_release);

    bool sensitive = !ready || (direction == TraceDirection::frontend && kind == 'p') ||
      (direction == TraceDirection::backend && (kind == 'R' || kind == 'K'));
    bool redacted = sensitive || configuration.content == TraceContent::metadata;
    auto payload = redacted ? std::span<const std::byte>{}
                            : body.first((std::min)(body.size(), configuration.payload_bytes));
    TraceMessage message{direction, kind, length, payload, redacted, !redacted && payload.size() != body.size()};
    configuration.handler(message);

    invoking.store(false, std::memory_order_release);
    active = previous;
  }
};

struct Connection::Impl {
  Transport transport;
  std::unique_ptr<detail::ConnectionEvents> events;
  Options options;
  OptionsInfo configuration;
  std::atomic<bool> busy{false};
  std::atomic<bool> copy_writing{false};
  Semaphore copy_send{1};
  std::atomic<std::size_t> borrowers{0};
  bool connected = false;
  bool authenticated = false;
  bool trace_ready = false;
  bool awaiting_ready = false;
  Transaction transaction = Transaction::idle;
  Endpoint endpoint;
  std::string local_address;
  std::optional<u64> peer_user;
  u32 process = 0;
  u32 protocol_minor = 2;
  Authentication authentication = Authentication::none;
  AuthenticationInfo authentication_info;
  detail::Bytes cancel_key;
  std::map<std::string, std::string, std::less<>> server_parameters;
  std::size_t parameter_bytes = 0;
  std::size_t notice_bytes = 0;
  std::size_t notification_bytes = 0;
  Diagnostic error;
  std::error_code operation_error;
  std::vector<Diagnostic> notices;
  std::shared_ptr<NoticeReceiver> notice_receiver;
  std::shared_ptr<NotificationReceiver> notification_receiver;
  std::shared_ptr<TraceReceiver> trace_receiver;
  std::vector<Notification> notifications;
  std::array<u32, static_cast<std::size_t>(detail::LargeObjectFunction::count)> large_object_functions{};
  CopyMode copy = CopyMode::none;
  u64 copy_generation = 0;
  PipelineState *pipeline = nullptr;
  std::optional<Results> copied;
  bool copy_send_done = false;
  bool copy_receive_done = false;
  std::vector<Column> streaming_columns;
  bool streaming_description = false;
  std::array<std::byte, 65536> input;
  std::size_t begin = 0;
  std::size_t end = 0;

  Impl(Transport stream, detail::OwnedOptions configuration)
      : transport(std::move(stream)), options(std::move(configuration).take())
  {
  }

  ~Impl()
  {
    weave::detail::require(
      !busy.load(std::memory_order_acquire) && !copy_writing.load(std::memory_order_acquire) &&
      borrowers.load(std::memory_order_acquire) == 0);
    if (events)
      events->destroy();
    detail::clear_passwords(options);
    OPENSSL_cleanse(cancel_key.data(), cancel_key.size());
  }

  struct Guard {
    Impl &connection;
    bool acquired = false;
    bool completed = false;
    bool sender = false;
    const std::error_code *failure = nullptr;

    struct Capture {
      Guard &guard;

      bool await_ready() const noexcept
      {
        return false;
      }

      template <class P>
        requires std::derived_from<P, weave::detail::PromiseBase>
      bool await_suspend(std::coroutine_handle<P> handle) const noexcept
      {
        guard.failure = &handle.promise().error;
        return false;
      }

      void await_resume() const noexcept
      {
      }
    };

    Capture capture() noexcept
    {
      return {*this};
    }

    Result<void> acquire()
    {
      if (connection.events && connection.events->invoking())
        return std::unexpected(make_error_code(Error::busy));
      auto &active = sender ? connection.copy_writing : connection.busy;
      if (active.exchange(true, std::memory_order_acq_rel))
        return std::unexpected(make_error_code(Error::busy));

      acquired = true;
      if (!connection.connected)
        return std::unexpected(make_error_code(Error::closed));

      return {};
    }

    ~Guard()
    {
      if (!acquired)
        return;

      // Failed chains are reclaimed inside-out. This guard dies before its own
      // promise, so its bound error slot still contains the propagated cause.
      if (failure && *failure)
        connection.record_failure(*failure);

      // A failed or cancelled protocol exchange cannot be reused without a drain.
      if (!completed)
        static_cast<void>(connection.close_transport());

      auto &active = sender ? connection.copy_writing : connection.busy;
      active.store(false, std::memory_order_release);
    }
  };

  void record_failure(std::error_code failure) noexcept
  {
    // Keep a terminal cause when a sibling drains with cancellation or a later
    // call observes the already-closed transport. SQL errors remain recoverable.
    bool sql = &operation_error.category() == &sql_error("XX000").category();
    if (!operation_error || sql || operation_error == std::errc::operation_canceled)
      operation_error = failure;
  }

  void observe_result(ResultSet &result)
  {
    if (events)
      events->create(result);
  }

  Task<std::size_t> read(std::span<std::byte> buffer)
  {
    return std::visit(
      [buffer](auto &stream) {
        return stream.read(buffer);
      },
      transport);
  }

  Task<void> write(std::span<const std::byte> buffer, bool untagged = false, bool sensitive = false)
  {
    if (connected && !untagged && !buffer.empty()) {
      // Busy guards also cover metadata and notification reads. Only protocol
      // commands make the last ReadyForQuery transaction state provisional.
      switch (std::to_integer<char>(buffer.front())) {
      case 'Q':
      case 'P':
      case 'B':
      case 'D':
      case 'E':
      case 'C':
      case 'F':
      case 'S':
        awaiting_ready = true;
        break;
      default:
        break;
      }
    }

    if (trace_receiver) {
      auto frames = buffer;
      while (!frames.empty()) {
        detail::Reader header{frames};
        char kind = untagged ? 0 : static_cast<char>(header.integer(1));
        auto length = header.integer();
        auto tag_bytes = untagged ? 0u : 1u;
        weave::detail::require(header.valid() && length >= 4 && length <= frames.size() - tag_bytes);
        auto body = frames.subspan(4 + tag_bytes, length - 4);
        trace_receiver->deliver(TraceDirection::frontend, kind, length, body, trace_ready && !sensitive);
        frames = frames.subspan(length + tag_bytes);
      }
    }
    return std::visit(
      [buffer](auto &stream) {
        return stream.write_all(buffer);
      },
      transport);
  }

  Result<void> close_transport() noexcept
  {
    if (pipeline)
      pipeline->stop(make_error_code(Error::closed));
    connected = false;
    return std::visit(
      [](auto &stream) {
        return stream.close();
      },
      transport);
  }

  Task<void> fill()
  {
    if (begin != end)
      co_await fail(Error::protocol);

    begin = 0;
    end = co_await read(input);
    if (!end)
      co_await fail(std::errc::connection_reset);
  }

  Task<void> read_exactly(std::span<std::byte> bytes)
  {
    while (!bytes.empty()) {
      if (begin == end)
        co_await fill();

      auto size = (std::min)(bytes.size(), end - begin);
      std::memcpy(bytes.data(), input.data() + begin, size);
      begin += size;
      bytes = bytes.subspan(size);
    }
  }

  Task<detail::Message> receive(bool sensitive = false)
  {
    std::array<std::byte, 5> header;
    co_await read_exactly(header);
    detail::Reader reader{header};
    detail::Message message;
    message.kind = static_cast<char>(reader.integer(1));
    auto size = reader.integer();
    if (size < 4)
      co_await fail(Error::protocol);
    if (size - 4 > options.limits.message_bytes)
      co_await fail(Error::resource_limit);

    message.body.resize(size - 4);
    co_await read_exactly(message.body);
    if (trace_receiver)
      trace_receiver->deliver(TraceDirection::backend, message.kind, size, message.body, trace_ready && !sensitive);
    co_return message;
  }

  Result<bool> administrative(const detail::Message &message)
  {
    if (message.kind == 'S') {
      detail::Reader reader{message.body};
      auto name = reader.string();
      auto value = reader.string();
      if (!reader.empty() || name.empty())
        return std::unexpected(make_error_code(Error::protocol));

      auto previous = server_parameters.find(name);
      auto old_size = previous == server_parameters.end() ? 0 : previous->first.size() + previous->second.size();
      auto new_size = name.size() + value.size();
      if (new_size > options.limits.result_bytes - (parameter_bytes - old_size) ||
        (previous == server_parameters.end() && server_parameters.size() >= 1024))
        return std::unexpected(make_error_code(Error::resource_limit));

      parameter_bytes = parameter_bytes - old_size + new_size;
      server_parameters.insert_or_assign(std::move(name), std::move(value));
      return true;
    }

    if (message.kind == 'N') {
      auto notice = detail::diagnostic(message.body);
      if (!notice)
        return std::unexpected(notice.error());
      if (notice_receiver) {
        notice_receiver->deliver(*notice);
        return true;
      }
      if (notices.size() >= options.limits.queued_notifications ||
        message.body.size() > options.limits.result_bytes - notice_bytes)
        return std::unexpected(make_error_code(Error::resource_limit));

      notice_bytes += message.body.size();
      notices.push_back(std::move(*notice));
      return true;
    }

    if (message.kind == 'A') {
      auto notification = detail::notification(message.body);
      if (!notification)
        return std::unexpected(notification.error());
      if (notification_receiver) {
        notification_receiver->deliver(*notification);
        return true;
      }
      if (notifications.size() >= options.limits.queued_notifications ||
        message.body.size() > options.limits.result_bytes - notification_bytes)
        return std::unexpected(make_error_code(Error::resource_limit));

      notification_bytes += message.body.size();
      notifications.push_back(std::move(*notification));
      return true;
    }

    return false;
  }

  Result<void> ready(const detail::Message &message)
  {
    if (message.body.size() != 1)
      return std::unexpected(make_error_code(Error::protocol));

    switch (std::to_integer<char>(message.body[0])) {
    case 'I':
      transaction = Transaction::idle;
      break;
    case 'T':
      transaction = Transaction::active;
      break;
    case 'E':
      transaction = Transaction::failed;
      break;
    default:
      return std::unexpected(make_error_code(Error::protocol));
    }

    awaiting_ready = false;
    return {};
  }

  Task<void> startup_impl(
    detail::GssSession *gss,
    OAuthAttempt *oauth,
    bool probe,
    ConnectionHistory *history,
    ConnectProgress &attempt)
  {
    struct Capture {
      Impl &connection;
      ConnectionHistory *history;

      ~Capture()
      {
        connection.authentication_info.method = connection.authentication;
        if (history)
          history->authentication(connection.authentication_info);
      }
    } capture{*this, history};

    if (options.gss_encryption != GssEncryption::disable) {
      if (history)
        history->stage(ConnectionStage::gss);
      detail::GssOptions policy;
      policy.service = options.gss_service;
      policy.delegate = options.gss_delegation;
      auto secured = co_await detail::gss_client(
        std::move(std::get<TcpStream>(transport)),
        *options.gss,
        options.host,
        std::move(policy),
        options.gss_encryption,
        error,
        options.channel_binding == ChannelBinding::require);
      std::visit(
        [&](auto &stream) {
          using Stream = std::remove_reference_t<decltype(stream)>;
          transport.emplace<Stream>(std::move(stream));
        },
        secured);
    }
    const bool encrypted = std::holds_alternative<detail::GssStream>(transport);
    if (encrypted && options.tls_options) {
      detail::clear_tls_credentials(*options.tls_options);
      options.tls_options.reset();
    }
    const auto mode = tls_mode(options);
    const bool plaintext_first = mode == TlsMode::allow;
    if (mode != TlsMode::disable && !plaintext_first && !encrypted) {
      if (history)
        history->stage(ConnectionStage::tls);
      if (options.client_certificate == TlsCertificateMode::disable && options.tls_options) {
        auto &identity = *options.tls_options;
        detail::clear_tls_credentials(identity);
        identity.certificate_file.clear();
        identity.private_key_file.clear();
        identity.private_key_password.clear();
        identity.private_key_format = TlsPrivateKeyFormat::pem;
      }
      auto identity = options.tls_options.value_or(TlsClientOptions{});
      if (mode != TlsMode::verify_full) {
        identity.verification = mode == TlsMode::verify_ca || !identity.ca_file.empty() ||
            !identity.ca_directory.empty()
          ? TlsVerification::certificate
          : TlsVerification::none;
      }
      auto credentials = options.tls ? Result<TlsContext>{*options.tls} : TlsContext::client(std::move(identity));
      detail::clear_tls_credentials(identity);
      if (options.tls_options) {
        detail::clear_tls_credentials(*options.tls_options);
        options.tls_options.reset();
      }
      if (!credentials) {
        // Application/provider errors are never endpoint availability evidence.
        attempt.credentials_failed = true;
        co_await fail(credentials.error());
      }
      const auto required_verification = mode == TlsMode::verify_full ? TlsVerification::hostname
        : mode == TlsMode::verify_ca                                  ? TlsVerification::certificate
                                                                      : TlsVerification::none;
      if (credentials->verification() < required_verification) {
        attempt.credentials_failed = true;
        co_await fail(std::errc::invalid_argument);
      }

      options.tls = *credentials;

      if (options.tls_negotiation == TlsNegotiation::direct && options.gss_encryption != GssEncryption::disable) {
        // Only an explicit GSS decline reaches here. Direct TLS must be the first bytes
        // on a fresh socket, pinned to the endpoint that declined GSS.
        attempt.direct_tls_pending = true;
        auto &socket = std::get<TcpStream>(transport);
        if (auto closed = socket.close(); !closed)
          co_await fail(closed.error());
        auto fresh = co_await tcp::connect(endpoint);
        if (auto configured = configure_socket(fresh, options); !configured)
          co_await fail(configured.error());
        transport.emplace<TcpStream>(std::move(fresh));
      }

      auto &socket = std::get<TcpStream>(transport);
      if (options.tls_negotiation == TlsNegotiation::postgres) {
        detail::Writer request;
        request.integer(8);
        request.integer(80877103);
        co_await write(request.bytes, true);
        std::array<std::byte, 1> response;
        co_await socket.read_exactly(response);
        if (response[0] == std::byte{'N'} && mode == TlsMode::prefer &&
          options.channel_binding != ChannelBinding::require &&
          options.client_certificate != TlsCertificateMode::require) {
          options.plaintext = true;
          options.tls.reset();
        } else if (response[0] != std::byte{'S'})
          co_await fail(Error::authentication);
      }
      if (!options.plaintext) {
        TlsHandshakeOptions handshake;
        handshake.timeout = options.connect_timeout;
        handshake.server_name_indication = options.server_name_indication;
        handshake.client_certificate = options.client_certificate;
        if (options.tls_negotiation == TlsNegotiation::direct) {
          handshake.required_protocol = "postgresql";
          attempt.direct_tls_pending = true;
        }

        auto secured = co_await as_result(
          tls::client(std::move(socket), *credentials, options.host, std::move(handshake)));
        if (!secured) {
          attempt.tls_handshake_failed = true;
          co_await fail(secured.error());
        }
        attempt.direct_tls_pending = false;
        transport.emplace<TlsStream<TcpStream>>(std::move(*secured));
      }
    }

    const bool required_identity = options.client_certificate == TlsCertificateMode::require;
    if (required_identity && !std::holds_alternative<TlsStream<TcpStream>>(transport)) {
      if (history)
        history->stage(ConnectionStage::tls);
      co_await fail(TlsError::client_certificate_required);
    }
    const bool verified_tls = options.tls && options.tls->verification() == TlsVerification::hostname &&
      std::holds_alternative<TlsStream<TcpStream>>(transport);

    if (history)
      history->stage(ConnectionStage::startup);
    detail::Writer startup;
    auto requested_protocol = static_cast<u32>(options.max_protocol);
    protocol_minor = requested_protocol & 0xffff;
    startup.integer(requested_protocol);
    startup.string("user");
    startup.string(options.user);
    startup.string("database");
    startup.string(options.database.empty() ? options.user : options.database);
    startup.string("application_name");
    startup.string(options.application_name);
    startup.string("client_encoding");
    startup.string(options.client_encoding);
    if (options.replication != Replication::disabled) {
      startup.string("replication");
      startup.string(options.replication == Replication::physical ? "true" : "database");
    }
    if (!options.server_options.empty()) {
      startup.string("options");
      startup.string(options.server_options);
    }
    for (const auto &[name, value] : options.settings) {
      startup.string(name);
      startup.string(value);
    }
    startup.integer(0, 1);
    if (startup.bytes.size() + 4 > options.limits.message_bytes)
      co_await fail(Error::resource_limit);
    detail::Writer request;
    request.integer(static_cast<u32>(startup.bytes.size() + 4));
    request.raw(startup.bytes);
    co_await write(request.bytes, true);

    detail::Scram scram;
    detail::OAuthExchange oauth_exchange;
    bool challenged = false;
    bool negotiated = false;
    std::vector<std::byte> binding;
    if (auto *secured = std::get_if<TlsStream<TcpStream>>(&transport)) {
      auto result = secured->channel_binding();
      if (result)
        binding = std::move(*result);
      else if (options.channel_binding == ChannelBinding::require)
        co_await fail(result.error());
    }

    for (;;) {
      auto message = co_await receive();
      auto handled = administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (*handled)
        continue;

      detail::Reader reader{message.body};
      if (message.kind == 'R') {
        if (authenticated)
          co_await fail(Error::protocol);

        auto method = reader.integer();
        if (!reader.valid())
          co_await fail(Error::protocol);

        if (probe) {
          if (auto valid = probe_authentication(method, reader); !valid)
            co_await fail(valid.error());
          co_await cancellation_point();
          co_return;
        }

        if (history)
          history->stage(ConnectionStage::authentication);
        detail::SecretWriter response;
        if (method == 0) {
          if (oauth && oauth->token && !challenged)
            co_await fail(Error::authentication);
          const bool native = authentication == Authentication::gss || authentication == Authentication::sspi;
          if (!reader.empty() || (scram.started() && !scram.verified()) || (native && (!gss || !gss->complete())) ||
            (!scram.verified() && options.channel_binding == ChannelBinding::require))
            co_await fail(Error::authentication);

          if (authentication == Authentication::oauth) {
            if (!oauth || !oauth->token)
              co_await fail(Error::authentication);
            if (auto accepted = oauth_exchange.accept(); !accepted)
              co_await fail(accepted.error());
          }

          if (!challenged && encrypted) {
            // Native GSS encryption can authenticate the login without a second GSS exchange.
            if (!allows_authentication(options.authentication, Authentication::none) &&
              !allows_authentication(options.authentication, Authentication::gss))
              co_await fail(Error::authentication);
            authentication = Authentication::gss;
          } else if (!allows_authentication(options.authentication, authentication)) {
            co_await fail(Error::authentication);
          }

          authenticated = true;
          authentication_info.complete = true;
          if (history)
            history->stage(ConnectionStage::startup);
          continue;
        }

        if (method != 8 && method != 11 && method != 12) {
          if (challenged)
            co_await fail(Error::protocol);
          if (oauth && oauth->token && method != 10)
            co_await fail(Error::authentication);

          challenged = true;
          if (method == 3)
            authentication = Authentication::password;
          else if (method == 5)
            authentication = Authentication::md5;
          else if (method == 10) {
            authentication = Authentication::scram_sha256;
            if (options.oauth && allows_authentication(options.authentication, Authentication::oauth)) {
              auto advertised = detail::oauth_offered(std::span{message.body}.subspan(4));
              if (!advertised)
                co_await fail(advertised.error());
              if (*advertised)
                authentication = Authentication::oauth;
            }
          } else if (method == 7)
            authentication = Authentication::gss;
          else if (method == 9)
            authentication = Authentication::sspi;
          else
            co_await fail(Error::unsupported_authentication);

          authentication_info.password_requested = password_requested(
            authentication,
            std::span{message.body}.subspan(4));
          authentication_info.password_missing = authentication_info.password_requested && options.password.empty();

          if (!allows_authentication(options.authentication, authentication))
            co_await fail(Error::authentication);
          if (oauth && oauth->token && authentication != Authentication::oauth)
            co_await fail(Error::authentication);
        } else if (method == 8) {
          const bool native = authentication == Authentication::gss || authentication == Authentication::sspi;
          if (!challenged || !native || !gss || gss->complete() || reader.empty())
            co_await fail(Error::protocol);
        } else if (!challenged ||
          (authentication != Authentication::scram_sha256 && authentication != Authentication::oauth)) {
          co_await fail(Error::protocol);
        }

        if (method == 7 || method == 9 || method == 8) {
          if (!gss || !options.gss)
            co_await fail(Error::unsupported_authentication);
          if (options.channel_binding == ChannelBinding::require)
            co_await fail(Error::authentication);
          if (!options.gss_mutual && !verified_tls)
            co_await fail(Error::authentication);
          if (method != 8 && !reader.empty())
            co_await fail(Error::protocol);

          auto host = detail::local_host(options.host) ? std::string{"localhost"} : options.host;
          detail::GssOptions policy;
          policy.service = options.gss_service;
          policy.delegate = options.gss_delegation;
          policy.require_mutual = options.gss_mutual;
          auto step = method == 8 ? gss->next(reader.rest())
                                  : gss->start(*options.gss, std::move(host), authentication, std::move(policy));
          auto token = co_await as_result(std::move(step));
          if (!token) {
            auto text = gss->diagnostic();
            if (!text.empty())
              error.fields.emplace_back('M', std::move(text));
            co_await fail(token.error());
          }
          if (token->bytes.empty())
            continue;
          response.raw(token->bytes);
        } else if (authentication == Authentication::oauth) {
          bool protected_transport = encrypted ||
            (verified_tls && std::holds_alternative<TlsStream<TcpStream>>(transport));
          if (!oauth || !options.oauth || !protected_transport || options.channel_binding == ChannelBinding::require)
            co_await fail(Error::authentication);
          if (method == 10) {
            if (!oauth->token) {
              auto &policy = *options.oauth;
              auto identity = detail::oauth_identity(policy.issuer);
              if (!identity)
                co_await fail(identity.error());
              if (identity->openid_configuration) {
                co_await cancellation_point();
                OAuthRequest cached_request{
                  .issuer = std::move(identity->issuer),
                  .client_id = policy.client_id,
                  .scope = policy.scope.value_or(std::string{}),
                  .openid_configuration = std::move(*identity->openid_configuration),
                  .host = options.host,
                  .port = endpoint.port,
                  .user = options.user,
                  .database = options.database.empty() ? options.user : options.database,
                  .client_secret = policy.client_secret,
                  .scope_explicit = policy.scope.has_value()};
                auto cached = policy.provider->cached_token(cached_request);
                if (!cached)
                  co_await fail(cached.error());
                co_await cancellation_point();
                if (*cached)
                  oauth->token.emplace(std::move(**cached));
              }
            }
            auto result = oauth->token ? oauth_exchange.start(reader.rest(), *oauth->token)
                                       : oauth_exchange.discover(reader.rest());
            if (!result)
              co_await fail(result.error());
            response = std::move(*result);
          } else if (method == 11) {
            auto discovery = detail::oauth_discovery(reader.rest(), *options.oauth);
            if (!discovery)
              co_await fail(discovery.error());
            auto result = oauth_exchange.reject();
            if (!result)
              co_await fail(result.error());
            response = std::move(*result);
            oauth->discovery.emplace(std::move(*discovery));
          } else {
            co_await fail(Error::protocol);
          }
        } else if (method == 10) {
          auto result = scram.start(options, reader.rest(), binding);
          if (!result)
            co_await fail(result.error());

          response = std::move(*result);
        } else if (method == 11) {
          auto result = scram.challenge(reader.rest(), options.limits.scram_iterations);
          if (!result)
            co_await fail(result.error());

          response = std::move(*result);
        } else if (method == 12) {
          auto result = scram.verify(reader.rest());
          if (!result)
            co_await fail(result.error());

          continue;
        } else if (method == 3 && options.allow_cleartext_password &&
          (encrypted ||
            (options.tls && options.tls->verification() == TlsVerification::hostname &&
              std::holds_alternative<TlsStream<TcpStream>>(transport))) &&
          !scram.started() && options.channel_binding != ChannelBinding::require && reader.empty()) {
          response.string(options.password);
        } else if (method == 5 && options.allow_md5_password && !scram.started() &&
          options.channel_binding != ChannelBinding::require) {
          auto result = detail::md5_password(options.user, options.password, reader.rest());
          if (!result)
            co_await fail(result.error());

          response.string({result->data(), result->size()});
        } else {
          co_await fail(Error::unsupported_authentication);
        }

        detail::SecretWriter password;
        password.message('p', response);
        auto sent = co_await as_result(write(password.bytes));
        OPENSSL_cleanse(response.bytes.data(), response.bytes.size());
        OPENSSL_cleanse(password.bytes.data(), password.bytes.size());
        if (!sent)
          co_await fail(sent.error());
      } else if (message.kind == 'K') {
        if (!authenticated || !cancel_key.empty())
          co_await fail(Error::protocol);

        process = reader.integer();
        auto key = reader.rest();
        if (!reader.valid() || key.size() < 4 || key.size() > 256 || (protocol_minor < 2 && key.size() != 4))
          co_await fail(Error::protocol);

        cancel_key.assign(key.begin(), key.end());
      } else if (message.kind == 'v') {
        auto minor = reader.integer();
        auto unsupported = reader.integer();
        bool invalid_minor = (minor != 0 && minor != 2) || minor > protocol_minor;
        if (!reader.empty() || invalid_minor || unsupported != 0 || authenticated || challenged || negotiated)
          co_await fail(Error::protocol);

        if (minor + 196608 < static_cast<u32>(options.min_protocol))
          co_await fail(std::errc::protocol_not_supported);

        protocol_minor = minor;
        negotiated = true;
      } else if (message.kind == 'E') {
        auto diagnostic = detail::diagnostic(message.body);
        if (!diagnostic)
          co_await fail(diagnostic.error());

        error = std::move(*diagnostic);
        if (authentication == Authentication::oauth && oauth && oauth->discovery && !oauth->token)
          co_return;
        co_await fail(sql_error(error.sqlstate()));
      } else if (message.kind == 'Z') {
        if (!authenticated)
          co_await fail(Error::protocol);

        auto result = ready(message);
        if (!result)
          co_await fail(result.error());

        connected = true;
        trace_ready = true;
        OPENSSL_cleanse(options.password.data(), options.password.size());
        options.password.clear();
        options.scram_client_key.reset();
        options.scram_server_key.reset();
        options.oauth.reset();
        co_return;
      } else {
        co_await fail(Error::protocol);
      }
    }
  }

  Task<void> startup_gss(OAuthAttempt *oauth, bool probe, ConnectionHistory *history, ConnectProgress &attempt)
  {
    detail::GssSession gss;
    auto result = co_await as_result(startup_impl(&gss, oauth, probe, history, attempt));
    co_await gss.close();
    if (probe || !result) {
      if (auto *secured = std::get_if<detail::GssStream>(&transport))
        co_await secured->finish();
    }
    if (!result)
      co_await fail(result.error());
  }

  Task<void> startup(OAuthAttempt *oauth, bool probe, ConnectionHistory *history, ConnectProgress &attempt)
  {
    // Ordinary startup keeps its original coroutine graph; provider cleanup is opt-in.
    return options.gss ? startup_gss(oauth, probe, history, attempt)
                       : startup_impl(nullptr, oauth, probe, history, attempt);
  }

  static Task<std::unique_ptr<Impl>> establish_once(
    detail::OwnedOptions pending,
    std::optional<IpAddress> address,
    Diagnostic *diagnostic,
    ConnectProgress &attempt,
    TargetSession target,
    std::mt19937 *random,
    std::shared_ptr<NoticeReceiver> notices,
    std::shared_ptr<TraceReceiver> trace,
    std::shared_ptr<NotificationReceiver> notifications,
    OAuthAttempt *oauth,
    bool probe = false,
    ConnectionHistory *history = nullptr)
  {
    auto &configuration = pending.value;
    PasswordCleanup cleanup{configuration};
    if (history)
      history->begin(configuration.host, configuration.port);
    if (history && diagnostic)
      *diagnostic = {};
    auto stream = co_await connect_transport(configuration, address, random, history);
    attempt.transport_connected = true;

    Endpoint endpoint;
    std::string local_address;
    std::optional<u64> peer_user;
    if (auto *socket = std::get_if<TcpStream>(&stream)) {
      if (history)
        history->stage(ConnectionStage::socket_options);
      if (auto configured = configure_socket(*socket, configuration); !configured)
        co_await fail(configured.error());
      auto destination = socket->peer_endpoint();
      if (!destination)
        co_await fail(destination.error());
      endpoint = *destination;
      if (history)
        history->endpoint(endpoint);
    } else {
      if (history)
        history->stage(ConnectionStage::peer_identity);
      auto &local = std::get<LocalStream>(stream);
      auto destination = local.peer_address();
      if (!destination)
        co_await fail(destination.error());
      if (destination->empty())
        co_await fail(Error::protocol);
      local_address = std::move(*destination);
#if !defined(_WIN32)
      auto peer = local.peer_credentials();
      if (!peer)
        co_await fail(peer.error());
      if (configuration.required_peer_user && peer->user != *configuration.required_peer_user)
        co_await fail(Error::authentication);
      peer_user = peer->user;
#endif
    }

    auto connection = std::make_unique<Impl>(std::move(stream), std::move(pending));
    connection->endpoint = endpoint;
    connection->local_address = std::move(local_address);
    connection->peer_user = peer_user;
    connection->notice_receiver = std::move(notices);
    connection->trace_receiver = std::move(trace);
    connection->notification_receiver = std::move(notifications);
    auto startup = co_await as_result(connection->startup(oauth, probe, history, attempt));
    if (!startup) {
      if (diagnostic)
        *diagnostic = connection->error;

      const auto mode = tls_mode(connection->options);
      const auto failure = startup.error();
      const bool stopped = failure == std::errc::operation_canceled || failure == std::errc::timed_out;
      const bool security_failure = failure == make_error_code(TlsError::certificate_verification) ||
        failure == make_error_code(TlsError::revocation) ||
        failure == make_error_code(TlsError::client_certificate_required);
      const bool transport_tls_failure = failure.category() == make_error_code(TlsError::protocol).category() ||
        std::string_view{failure.category().name()} == "openssl" || failure == std::errc::connection_reset ||
        failure == std::errc::connection_aborted || failure == std::errc::broken_pipe;
      const bool prefer_retry = mode == TlsMode::prefer && attempt.tls_handshake_failed && transport_tls_failure &&
        !security_failure;
      const bool allow_retry = mode == TlsMode::allow && connection->error.sqlstate() == "28000" &&
        !connection->authentication_info.password_requested && !connection->authenticated;
      const bool fallback_allowed = !stopped && !attempt.credentials_failed && !probe && !oauth &&
        connection->options.gss_encryption == GssEncryption::disable &&
        connection->options.channel_binding != ChannelBinding::require &&
        connection->options.client_certificate != TlsCertificateMode::require;
      if (fallback_allowed && (prefer_retry || allow_retry)) {
        if (history)
          history->fail(failure, &connection->error);
        detail::OwnedOptions retry{std::move(connection->options)};
        retry.value.tls_mode = prefer_retry ? TlsMode::disable : TlsMode::require;
        retry.value.plaintext = prefer_retry;
        if (prefer_retry) {
          retry.value.tls.reset();
          if (retry.value.tls_options) {
            detail::clear_tls_credentials(*retry.value.tls_options);
            retry.value.tls_options.reset();
          }
        }
        if (auto closed = connection->close_transport(); !closed)
          co_await fail(closed.error());
        notices = std::move(connection->notice_receiver);
        trace = std::move(connection->trace_receiver);
        notifications = std::move(connection->notification_receiver);
        connection.reset();
        attempt = {};
        if (diagnostic)
          *diagnostic = {};
        co_return co_await establish_once(
          std::move(retry),
          endpoint.address,
          diagnostic,
          attempt,
          target,
          nullptr,
          std::move(notices),
          std::move(trace),
          std::move(notifications),
          nullptr,
          false,
          history);
      }

      co_await fail(startup.error());
    }
    if (probe) {
      if (auto closed = connection->close_transport(); !closed)
        co_await fail(closed.error());
    }
    if (connection->connected && target != TargetSession::any) {
      if (history)
        history->stage(ConnectionStage::target_session);
      auto accepted = co_await as_result(connection->accepts(target));
      if (!accepted) {
        if (diagnostic)
          *diagnostic = connection->error;
        co_await fail(accepted.error());
      }
      if (!*accepted)
        co_await fail(Error::target_session);
    }

    if (history)
      history->complete();
    co_return connection;
  }

  static Task<std::unique_ptr<Impl>> establish_oauth(
    detail::OwnedOptions pending,
    std::optional<IpAddress> address,
    Diagnostic *diagnostic,
    ConnectProgress &attempt,
    TargetSession target,
    std::mt19937 *random,
    std::shared_ptr<NoticeReceiver> notices,
    std::shared_ptr<TraceReceiver> trace,
    std::shared_ptr<NotificationReceiver> notifications,
    ConnectionHistory *history)
  {
    auto deadline = pending.value.connect_timeout;
    OAuthAttempt oauth;
    auto discovery = co_await timeout(
      deadline,
      establish_once(
        std::move(pending),
        address,
        diagnostic,
        attempt,
        target,
        random,
        notices,
        trace,
        notifications,
        &oauth,
        false,
        history));
    if (!oauth.discovery)
      co_return discovery;

    detail::OwnedOptions reconnect{Options{discovery->options}};
    auto endpoint = discovery->endpoint;
    reconnect.value.port = endpoint.port;
    if (auto *secured = std::get_if<detail::GssStream>(&discovery->transport)) {
      reconnect.value.gss_encryption = GssEncryption::require;
      co_await secured->finish();
    } else {
      reconnect.value.gss_encryption = GssEncryption::disable;
    }
    if (auto closed = discovery->close_transport(); !closed)
      co_await fail(closed.error());
    discovery.reset();

    if (history)
      history->stage(ConnectionStage::oauth);
    auto &policy = *reconnect.value.oauth;
    auto identity = detail::oauth_identity(policy.issuer);
    if (!identity)
      co_await fail(identity.error());
    OAuthRequest request{
      .issuer = std::move(identity->issuer),
      .client_id = policy.client_id,
      .scope = std::move(oauth.discovery->scope),
      .openid_configuration = std::move(oauth.discovery->openid_configuration),
      .host = reconnect.value.host,
      .port = endpoint.port,
      .user = reconnect.value.user,
      .database = reconnect.value.database.empty() ? reconnect.value.user : reconnect.value.database,
      .client_secret = policy.client_secret,
      .scope_explicit = policy.scope.has_value()};
    auto token = co_await timeout(policy.acquisition_timeout, policy.provider->request(std::move(request)));
    if (token.value().empty())
      co_await fail(Error::authentication);
    oauth.token.emplace(std::move(token));
    oauth.discovery.reset();
    if (diagnostic)
      *diagnostic = {};

    auto connected = co_await timeout(
      deadline,
      establish_once(
        std::move(reconnect),
        endpoint.address,
        diagnostic,
        attempt,
        target,
        nullptr,
        notices,
        trace,
        notifications,
        &oauth,
        false,
        history));
    // A different method on the pinned reconnect must not silently replace OAuth.
    if (connected->authentication != Authentication::oauth || !connected->connected)
      co_await fail(Error::authentication);
    co_return connected;
  }

  static Task<std::unique_ptr<Impl>> establish(
    detail::OwnedOptions pending,
    std::optional<IpAddress> address,
    Diagnostic *diagnostic,
    ConnectProgress &attempt,
    TargetSession target,
    std::mt19937 *random,
    std::shared_ptr<NoticeReceiver> notices,
    std::shared_ptr<TraceReceiver> trace,
    std::shared_ptr<NotificationReceiver> notifications,
    bool probe = false,
    ConnectionHistory *history = nullptr)
  {
    auto operation = [&]() {
      if (pending.value.oauth && !probe)
        return establish_oauth(
          std::move(pending),
          address,
          diagnostic,
          attempt,
          target,
          random,
          notices,
          trace,
          notifications,
          history);
      auto deadline = pending.value.connect_timeout;
      return timeout(
        deadline,
        establish_once(
          std::move(pending),
          address,
          diagnostic,
          attempt,
          target,
          random,
          notices,
          trace,
          notifications,
          nullptr,
          probe,
          history));
    }();
    if (history) {
      return std::move(operation).on_error([history, diagnostic](std::error_code error) noexcept {
        history->fail(error, diagnostic);
      });
    }
    return operation;
  }

  Task<bool> accepts(TargetSession target)
  {
    if (target == TargetSession::any)
      co_return true;

    auto results = co_await query(
      "SELECT pg_catalog.current_setting('default_transaction_read_only'), pg_catalog.pg_is_in_recovery()");
    if (results.size() != 1 || results.front().rows.size() != 1 || results.front().rows.front().size() != 2 ||
      transaction != Transaction::idle)
      co_await fail(Error::protocol);

    const auto &row = results.front().rows.front();
    bool valid_read_only = !row[0].is_null() && (row[0].bytes() == "on" || row[0].bytes() == "off");
    bool valid_recovery = !row[1].is_null() && (row[1].bytes() == "t" || row[1].bytes() == "f");
    if (!valid_read_only || !valid_recovery)
      co_await fail(Error::protocol);

    bool standby = row[1].bytes() == "t";
    bool read_only = standby || row[0].bytes() == "on";
    switch (target) {
    case TargetSession::read_write:
      co_return !read_only;
    case TargetSession::read_only:
      co_return read_only;
    case TargetSession::primary:
      co_return !standby;
    case TargetSession::standby:
      co_return standby;
    default:
      co_await fail(std::errc::invalid_argument);
    }
  }

  template <bool RetainErrors = false, class Storage>
  Task<ExchangeResults<RetainErrors>> exchange(
    detail::BasicWriter<Storage> request,
    std::optional<std::size_t> expected = std::nullopt,
    ExchangeKind kind = ExchangeKind::command,
    std::optional<Encoding> requested_encoding = std::nullopt,
    Guard *lease = nullptr,
    bool sensitive = false)
  {
    co_await cancellation_point();
    if (copy != CopyMode::none)
      co_await fail(Error::busy);
    bool simple_query = !request.bytes.empty() && request.bytes.front() == std::byte{'Q'};
    if (options.replication != Replication::disabled && !simple_query)
      co_await fail(std::errc::operation_not_supported);
    if (request.bytes.size() > options.limits.message_bytes)
      co_await fail(Error::resource_limit);

    Guard guard{*this};
    if (lease) {
      weave::detail::require(&lease->connection == this && lease->acquired);
    } else if (auto result = guard.acquire(); !result) {
      co_await fail(result.error());
    }
    co_await guard.capture();
    auto &active_guard = lease ? *lease : guard;
    active_guard.completed = false;

    error = {};
    operation_error = {};
    co_await write(request.bytes, false, sensitive);
    ExchangeResults<RetainErrors> results;
    std::optional<ResultSet> current;
    bool description = false;
    std::error_code failure;
    std::size_t retained = 0;
    for (;;) {
      auto message = co_await receive(sensitive);
      auto handled = administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (*handled)
        continue;

      if (message.kind == 'E') {
        if (failure)
          co_await fail(Error::protocol);
        auto diagnostic = detail::diagnostic(message.body);
        if (!diagnostic)
          co_await fail(diagnostic.error());

        error = std::move(*diagnostic);
        failure = sql_error(error.sqlstate());
        if (failure == make_error_code(Error::protocol))
          co_await fail(Error::protocol);
        if constexpr (RetainErrors) {
          if (expected && results.size() >= *expected)
            co_await fail(Error::protocol);
          const auto bytes = message.body.size() + sizeof(Outcome) +
            error.fields.size() * sizeof(std::pair<char, std::string>);
          if (bytes > options.limits.result_bytes - retained)
            co_await fail(Error::resource_limit);
          retained += bytes;
          results.push_back({.error = error});
        }
        current.reset();
        description = false;
        continue;
      }

      if (message.kind == 'Z') {
        if (auto result = ready(message); !result)
          co_await fail(result.error());
        if (current && !failure) {
          if (kind != ExchangeKind::description && kind != ExchangeKind::portal_description)
            co_await fail(Error::protocol);

          current->kind = ResultKind::description;
          observe_result(*current);
          append_result<RetainErrors>(results, std::move(*current));
        }
        if (!failure && expected && results.size() != *expected)
          co_await fail(Error::protocol);
        if constexpr (RetainErrors) {
          if (results.empty())
            co_await fail(Error::protocol);
        }

        if (!failure && requested_encoding) {
          auto parameter = server_parameters.find("client_encoding");
          if (parameter == server_parameters.end())
            co_await fail(Error::protocol);
          auto actual = parse_encoding(parameter->second);
          if (!actual || *actual != *requested_encoding)
            co_await fail(Error::protocol);
        }

        active_guard.completed = true;
        if constexpr (!RetainErrors) {
          if (failure)
            co_await fail(failure);
        }

        co_return results;
      }

      if (failure)
        co_await fail(Error::protocol);

      retained += message.body.size() + sizeof(std::conditional_t<RetainErrors, Outcome, ResultSet>);
      if (retained > options.limits.result_bytes)
        co_await fail(Error::resource_limit);

      if (kind == ExchangeKind::portal_description) {
        if (current || (message.kind != 'T' && message.kind != 'n'))
          co_await fail(Error::protocol);

        current.emplace();
        current->kind = ResultKind::description;
        if (message.kind == 'T') {
          auto columns = detail::columns(message.body, detail::ResultAccess::schema(*current));
          if (!columns)
            co_await fail(columns.error());
          current->columns = std::move(*columns);
        } else if (!message.body.empty()) {
          co_await fail(Error::protocol);
        }
        continue;
      }

      if (message.kind == 'T') {
        if (description)
          co_await fail(Error::protocol);
        if (!current)
          current.emplace();

        auto columns = detail::columns(message.body, detail::ResultAccess::schema(*current));
        if (!columns)
          co_await fail(columns.error());

        current->columns = std::move(*columns);
        current->kind = ResultKind::tuples;
        description = true;
      } else if (message.kind == 'D') {
        if (!current || !description)
          co_await fail(Error::protocol);

        auto row = detail::row(message.body, current->columns, detail::ResultAccess::rows(*current));
        if (!row)
          co_await fail(row.error());

        retained += sizeof(Row) + row->size() * sizeof(Value);
        if (retained > options.limits.result_bytes)
          co_await fail(Error::resource_limit);

        current->rows.push_back(std::move(*row));
      } else if (message.kind == 'C') {
        if (!current)
          current.emplace();

        detail::Reader reader{message.body};
        current->command = reader.string();
        if (!reader.empty())
          co_await fail(Error::protocol);

        current->kind = description ? ResultKind::tuples : ResultKind::command;
        observe_result(*current);
        append_result<RetainErrors>(results, std::move(*current));
        current.reset();
        description = false;
      } else if (message.kind == 's') {
        if (!message.body.empty() || !current || kind != ExchangeKind::fetch)
          co_await fail(Error::protocol);

        current->suspended = true;
        current->kind = ResultKind::row_chunk;
        observe_result(*current);
        append_result<RetainErrors>(results, std::move(*current));
        current.reset();
        description = false;
      } else if (message.kind == 'I') {
        if (!message.body.empty() || current)
          co_await fail(Error::protocol);

        ResultSet empty;
        empty.kind = ResultKind::empty_query;
        observe_result(empty);
        append_result<RetainErrors>(results, std::move(empty));
      } else if (message.kind == 't') {
        if (kind != ExchangeKind::description)
          co_await fail(Error::protocol);
        if (!current)
          current.emplace();
        current->kind = ResultKind::description;

        detail::Reader reader{message.body};
        auto count = reader.integer(2);
        for (u32 index = 0; index < count && reader.valid(); ++index)
          current->parameter_types.push_back(reader.integer());
        if (!reader.empty())
          co_await fail(Error::protocol);
      } else if (message.kind == 'n' || message.kind == '1' || message.kind == '2' || message.kind == '3') {
        if (!message.body.empty())
          co_await fail(Error::protocol);
        if (message.kind == 'n' && kind == ExchangeKind::description && !current) {
          current.emplace();
          current->kind = ResultKind::description;
        }
      } else if (message.kind == 'G' || message.kind == 'H' || message.kind == 'W') {
        co_await fail(Error::unexpected_copy);
      } else {
        co_await fail(Error::protocol);
      }
    }
  }

  template <bool RetainErrors = false>
  Task<ExchangeResults<RetainErrors>> query(std::string sql, [[maybe_unused]] Borrow borrow = {})
  {
    if (sql.size() > options.limits.message_bytes - 1)
      co_await fail(Error::resource_limit);
    if (!detail::cstring_valid(sql))
      co_await fail(std::errc::invalid_argument);

    detail::Writer body;
    body.string(sql);
    detail::Writer request;
    request.message('Q', body);
    co_return co_await exchange<RetainErrors>(std::move(request));
  }

  Task<void> set_encoding(Encoding encoding, [[maybe_unused]] Borrow borrow)
  {
    auto info = encoding_info(encoding);
    if (!info)
      co_await fail(info.error());

    detail::Writer body;
    body.string("SET client_encoding TO '" + std::string{info->name} + "'");
    detail::Writer request;
    request.message('Q', body);
    co_await exchange(std::move(request), 1, ExchangeKind::command, encoding);
  }

  template <bool Change>
  Task<std::conditional_t<Change, ResultSet, std::string>> password(
    std::string user,
    Result<detail::SecretText> password,
    PasswordOptions configuration,
    [[maybe_unused]] Borrow borrow)
  {
    co_await cancellation_point();
    if (!password)
      co_await fail(password.error());
    if (user.size() > 65536 || !detail::cstring_valid(user) || configuration.iterations == 0 ||
      configuration.iterations > 1000000)
      co_await fail(std::errc::invalid_argument);
    if (configuration.algorithm && *configuration.algorithm != PasswordAlgorithm::scram_sha256 &&
      *configuration.algorithm != PasswordAlgorithm::md5)
      co_await fail(std::errc::invalid_argument);

    Guard guard{*this};
    if (auto acquired = guard.acquire(); !acquired)
      co_await fail(acquired.error());
    guard.completed = true;
    if (copy != CopyMode::none)
      co_await fail(Error::busy);
    co_await guard.capture();

    auto algorithm = configuration.algorithm;
    if (!algorithm) {
      detail::Writer body;
      body.string("SHOW password_encryption");
      detail::Writer request;
      request.message('Q', body);
      auto policy = co_await exchange(std::move(request), 1, ExchangeKind::command, std::nullopt, &guard);
      bool valid_shape = policy.front().columns.size() == 1 && policy.front().rows.size() == 1 &&
        policy.front().rows.front().size() == 1 && !policy.front().rows.front().front().is_null() &&
        policy.front().rows.front().front().format == Format::text;
      if (!valid_shape) {
        guard.completed = false;
        co_await fail(Error::protocol);
      }
      auto value = policy.front().rows.front().front().bytes();
      if (value == "scram-sha-256")
        algorithm = PasswordAlgorithm::scram_sha256;
      else if (value == "md5" || value == "on" || value == "off")
        algorithm = PasswordAlgorithm::md5;
      else
        co_await fail(std::errc::operation_not_supported);
      if (*algorithm == PasswordAlgorithm::md5 && !configuration.allow_md5)
        co_await fail(std::errc::operation_not_supported);
    }

    std::string quoted_user;
    if constexpr (Change) {
      // The policy exchange may have delivered a new client_encoding ParameterStatus.
      auto parameter = server_parameters.find("client_encoding");
      if (parameter == server_parameters.end())
        co_await fail(Error::protocol);
      auto encoding = parse_encoding(parameter->second);
      if (!encoding)
        co_await fail(std::errc::operation_not_supported);
      auto quoted = pg::escape_identifier(user, *encoding, options.limits.message_bytes);
      if (!quoted)
        co_await fail(quoted.error());
      quoted_user = std::move(*quoted);
    }

    std::string_view cleartext{password->data(), password->size()};
    auto verifier = detail::password_verifier(cleartext, user, *algorithm, configuration.iterations);
    if (!verifier)
      co_await fail(verifier.error());
    detail::SecretText{}.swap(*password);
    co_await cancellation_point();
    if constexpr (!Change) {
      co_return std::string{verifier->begin(), verifier->end()};
    } else {
      detail::SecretWriter body;
      body.raw("ALTER USER ");
      body.raw(quoted_user);
      body.raw(" PASSWORD '");
      body.raw(std::string_view{verifier->data(), verifier->size()});
      body.raw("'");
      body.integer(0, 1);
      detail::SecretWriter request;
      request.message('Q', body);
      auto results = co_await exchange(std::move(request), 1, ExchangeKind::command, std::nullopt, &guard, true);
      co_return std::move(results.front());
    }
  }

  template <bool RetainErrors = false>
  Task<std::conditional_t<RetainErrors, Outcome, ResultSet>> execute(
    std::string sql,
    std::vector<Parameter> parameters,
    Format format,
    bool prepared,
    [[maybe_unused]] Borrow borrow = {})
  {
    if (sql.size() > options.limits.message_bytes - 16 || parameters.size() > 65535)
      co_await fail(Error::resource_limit);
    detail::Writer request;
    if (!prepared) {
      std::vector<u32> types;
      for (const auto &parameter : parameters)
        types.push_back(parameter.type);

      auto parsed = parse("", sql, types);
      if (!parsed)
        co_await fail(parsed.error());

      request = std::move(*parsed);
    }

    auto bound = detail::bind(
      request,
      prepared ? std::string_view{sql} : std::string_view{},
      parameters,
      format,
      options.limits.message_bytes);
    if (!bound)
      co_await fail(bound.error());

    execution(request);
    auto results = co_await exchange<RetainErrors>(std::move(request), 1);
    if (results.size() != 1)
      co_await fail(Error::protocol);

    co_return std::move(results.front());
  }

  Task<void> prepare(std::string name, std::string sql, std::vector<u32> types, [[maybe_unused]] Borrow borrow = {})
  {
    if (types.size() > 65535 || name.size() > options.limits.message_bytes / 2 ||
      sql.size() > options.limits.message_bytes / 2)
      co_await fail(Error::resource_limit);
    auto parsed = parse(name, sql, types);
    if (!parsed)
      co_await fail(parsed.error());

    parsed->message('S');
    co_await exchange(std::move(*parsed), 0);
  }

  Task<ResultSet> describe(std::string name, bool portal, [[maybe_unused]] Borrow borrow = {})
  {
    if (name.size() > options.limits.message_bytes - 8)
      co_await fail(Error::resource_limit);
    if (!detail::cstring_valid(name))
      co_await fail(std::errc::invalid_argument);

    detail::Writer body;
    body.integer(portal ? 'P' : 'S', 1);
    body.string(name);
    detail::Writer request;
    request.message('D', body);
    request.message('S');
    const auto kind = portal ? ExchangeKind::portal_description : ExchangeKind::description;
    auto results = co_await exchange(std::move(request), 1, kind);
    if (results.size() != 1)
      co_await fail(Error::protocol);

    co_return std::move(results.front());
  }

  Task<void> close_prepared(std::string name, [[maybe_unused]] Borrow borrow = {})
  {
    if (name.size() > options.limits.message_bytes - 8)
      co_await fail(Error::resource_limit);
    if (!detail::cstring_valid(name))
      co_await fail(std::errc::invalid_argument);

    detail::Writer body;
    body.integer('S', 1);
    body.string(name);
    detail::Writer request;
    request.message('C', body);
    request.message('S');
    co_await exchange(std::move(request), 0);
  }

  Task<void> receive_batch(std::vector<Outcome> &outcomes)
  {
    std::size_t index = 0;
    std::size_t retained = 0;
    bool failed = false;
    std::optional<ResultSet> current;

    for (;;) {
      auto message = co_await receive();
      auto handled = administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (*handled)
        continue;

      if (message.kind == 'Z') {
        if (auto result = ready(message); !result)
          co_await fail(result.error());
        if (!failed && index != outcomes.size())
          co_await fail(Error::protocol);

        while (index < outcomes.size())
          outcomes[index++].aborted = true;

        co_return;
      }

      if (failed || index >= outcomes.size())
        co_await fail(Error::protocol);

      retained += message.body.size() + sizeof(ResultSet);
      if (retained > options.limits.result_bytes)
        co_await fail(Error::resource_limit);

      if (message.kind == 'E') {
        auto diagnostic = detail::diagnostic(message.body);
        if (!diagnostic)
          co_await fail(diagnostic.error());

        if (sql_error(diagnostic->sqlstate()) == make_error_code(Error::protocol))
          co_await fail(Error::protocol);

        error = *diagnostic;
        outcomes[index++].error = std::move(*diagnostic);
        current.reset();
        failed = true;
      } else if (message.kind == 'T') {
        if (current)
          co_await fail(Error::protocol);

        current.emplace();
        auto columns = detail::columns(message.body, detail::ResultAccess::schema(*current));
        if (!columns)
          co_await fail(columns.error());

        current->columns = std::move(*columns);
        current->kind = ResultKind::tuples;
      } else if (message.kind == 'D') {
        if (!current)
          co_await fail(Error::protocol);

        auto row = detail::row(message.body, current->columns, detail::ResultAccess::rows(*current));
        if (!row)
          co_await fail(row.error());

        retained += sizeof(Row) + row->size() * sizeof(Value);
        if (retained > options.limits.result_bytes)
          co_await fail(Error::resource_limit);

        current->rows.push_back(std::move(*row));
      } else if (message.kind == 'C' || message.kind == 'I') {
        if (!current)
          current.emplace();

        detail::Reader reader{message.body};
        if (message.kind == 'C')
          current->command = reader.string();
        if (!reader.empty())
          co_await fail(Error::protocol);

        if (message.kind == 'I')
          current->kind = ResultKind::empty_query;
        else if (current->kind != ResultKind::tuples)
          current->kind = ResultKind::command;
        observe_result(*current);
        outcomes[index++].result = std::move(*current);
        current.reset();
      } else if (message.kind == 'n' || message.kind == '1' || message.kind == '2') {
        if (!message.body.empty())
          co_await fail(Error::protocol);
      } else {
        co_await fail(Error::protocol);
      }
    }
  }

  Task<std::vector<Outcome>> batch(std::vector<Command> commands, [[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (copy != CopyMode::none)
      co_await fail(Error::busy);
    if (options.replication != Replication::disabled)
      co_await fail(std::errc::operation_not_supported);
    if (commands.empty())
      co_return std::vector<Outcome>{};
    if (commands.size() > 65535)
      co_await fail(Error::resource_limit);

    detail::Writer request;
    for (const auto &command : commands) {
      if (command.sql.size() > options.limits.message_bytes - 16 || command.parameters.size() > 65535)
        co_await fail(Error::resource_limit);

      std::vector<u32> types;
      for (const auto &parameter : command.parameters)
        types.push_back(parameter.type);

      auto parsed = parse("", command.sql, types);
      if (!parsed)
        co_await fail(parsed.error());

      detail::Writer statement = std::move(*parsed);
      if (auto bound = detail::bind(statement, "", command.parameters, command.format, options.limits.message_bytes);
        !bound)
        co_await fail(bound.error());

      execution(statement);
      // One Sync covers the batch, preserving PostgreSQL's abort-until-Sync semantics.
      statement.bytes.resize(statement.bytes.size() - 5);
      if (statement.bytes.size() > options.limits.message_bytes ||
        statement.bytes.size() > options.limits.result_bytes - request.bytes.size())
        co_await fail(Error::resource_limit);

      request.raw(statement.bytes);
    }

    request.message('S');
    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    error = {};
    operation_error = {};
    std::vector<Outcome> outcomes(commands.size());
    // Read while sending: either side may otherwise fill the peer's socket buffers.
    std::error_code failure;
    auto cancel_peer = [this, &failure](weave::Error error) noexcept {
      if (!failure)
        failure = error;
      connected = false;
      static_cast<void>(std::visit(
        [](auto &stream) {
          return stream.cancel();
        },
        transport));
    };
    auto completed = co_await as_result(
      when_all(write(request.bytes).on_error(cancel_peer), receive_batch(outcomes).on_error(cancel_peer)));
    // Argument-order join errors must not mask the cause with induced sibling cancellation.
    if (!completed)
      co_await fail(failure ? failure : completed.error());
    guard.completed = true;
    co_return outcomes;
  }

  Task<void> receive_pipeline(PipelineState &pipeline_state, std::vector<PipelineEntry> entries)
  {
    bool streaming = std::ranges::any_of(entries, [](const auto &entry) {
      return entry.chunk_rows != 0;
    });
    bool aborted;
    {
      std::lock_guard lock(pipeline_state.mutex);
      aborted = pipeline_state.aborted;
    }

    for (const auto &entry : entries) {
      PipelineBuffer buffer{pipeline_state, entry, streaming};
      co_await buffer.initialize();
      auto &delivery = buffer.delivery;

      if (aborted && entry.kind != PipelineKind::sync) {
        delivery.result.outcome.result.reset();
        delivery.result.outcome.aborted = true;
        co_await buffer.publish();
        continue;
      }

      auto step = entry.step;
      bool completed = false;
      bool row_description = false;

      while (!completed) {
        auto message = co_await receive();
        auto handled = administrative(message);
        if (!handled)
          co_await fail(handled.error());
        if (*handled)
          continue;

        if (entry.chunk_rows && message.kind == 'C' && !delivery.result.outcome.result->rows.empty())
          co_await buffer.emit();

        if (entry.chunk_rows && (message.kind == 'C' || message.kind == 'E')) {
          pipeline_state.release(std::exchange(buffer.schema_bytes, 0));
          buffer.schema.clear();
        }

        if (entry.chunk_rows && message.kind == 'E') {
          // Earlier chunks remain valid events; an unfinished chunk is discarded.
          delivery = {};
          co_await buffer.initialize(false);
        }

        if (message.kind != 'D')
          co_await buffer.reserve(message.body.size());

        if (message.kind == 'E' && entry.kind != PipelineKind::sync) {
          auto diagnostic = detail::diagnostic(message.body);
          if (!diagnostic)
            co_await fail(diagnostic.error());
          if (sql_error(diagnostic->sqlstate()) == make_error_code(Error::protocol))
            co_await fail(Error::protocol);

          error = *diagnostic;
          delivery.result.outcome.result.reset();
          delivery.result.outcome.error = std::move(*diagnostic);
          aborted = true;
          completed = true;
          continue;
        }

        if (message.kind == 'G' || message.kind == 'H' || message.kind == 'W')
          co_await fail(Error::unexpected_copy);

        switch (step) {
        case PipelineStep::parse:
          if (message.kind != '1' || !message.body.empty())
            co_await fail(Error::protocol);

          completed = entry.kind == PipelineKind::prepare;
          step = PipelineStep::bind;
          break;

        case PipelineStep::bind:
          if (message.kind != '2' || !message.body.empty())
            co_await fail(Error::protocol);

          step = PipelineStep::description;
          break;

        case PipelineStep::parameters: {
          if (message.kind != 't')
            co_await fail(Error::protocol);

          detail::Reader reader{message.body};
          auto count = reader.integer(2);
          auto &types = delivery.result.outcome.result->parameter_types;
          for (u32 index = 0; index < count && reader.valid(); ++index)
            types.push_back(reader.integer());
          if (!reader.empty())
            co_await fail(Error::protocol);

          step = PipelineStep::description;
          break;
        }

        case PipelineStep::description: {
          delivery.result.outcome.result->kind = entry.kind == PipelineKind::describe ? ResultKind::description
                                                                                      : ResultKind::command;
          if (message.kind == 'T') {
            auto columns = detail::columns(message.body, detail::ResultAccess::schema(*delivery.result.outcome.result));
            if (!columns)
              co_await fail(columns.error());
            auto metadata = columns->size() * sizeof(Column);
            co_await buffer.reserve(metadata);

            delivery.result.outcome.result->columns = std::move(*columns);
            if (entry.chunk_rows) {
              auto size = message.body.size() + metadata;
              auto staged = delivery.retained + buffer.schema_bytes;
              co_await pipeline_state.reserve(size, buffer.schema_bytes, staged, streaming);
              buffer.schema = static_cast<std::vector<Column>>(delivery.result.outcome.result->columns);
            }
            row_description = true;
            if (entry.kind != PipelineKind::describe)
              delivery.result.outcome.result->kind = ResultKind::tuples;
          } else if (message.kind != 'n' || !message.body.empty()) {
            co_await fail(Error::protocol);
          }

          completed = entry.kind == PipelineKind::describe;
          step = PipelineStep::rows;
          break;
        }

        case PipelineStep::rows: {
          if (message.kind == 'D' && row_description) {
            auto row = detail::row(
              message.body,
              delivery.result.outcome.result->columns,
              detail::ResultAccess::rows(*delivery.result.outcome.result));
            if (!row)
              co_await fail(row.error());
            auto size = message.body.size() + sizeof(Row) + row->size() * sizeof(Value);
            co_await buffer.reserve(size);

            // Reservation can publish a full batch and replace its owning storage.
            // Do not let the first row of the next batch retain the previous pool.
            auto arena = detail::ResultAccess::rows(*delivery.result.outcome.result);
            if (row->get_allocator().arena().identity() != arena.identity()) {
              row = detail::row(message.body, delivery.result.outcome.result->columns, std::move(arena));
              if (!row)
                co_await fail(row.error());
            }

            auto &rows = delivery.result.outcome.result->rows;
            rows.push_back(std::move(*row));
            if (entry.chunk_rows && rows.size() >= entry.chunk_rows)
              co_await buffer.emit();
          } else if (message.kind == 'C') {
            detail::Reader reader{message.body};
            delivery.result.outcome.result->command = reader.string();
            if (!reader.empty())
              co_await fail(Error::protocol);

            delivery.result.outcome.result->kind = row_description ? ResultKind::tuples : ResultKind::command;
            completed = true;
          } else if (message.kind == 'I' && !row_description && message.body.empty()) {
            delivery.result.outcome.result->kind = ResultKind::empty_query;
            completed = true;
          } else {
            co_await fail(Error::protocol);
          }
          break;
        }

        case PipelineStep::close:
          if (message.kind != '3' || !message.body.empty())
            co_await fail(Error::protocol);

          completed = true;
          break;

        case PipelineStep::sync:
          if (message.kind != 'Z')
            co_await fail(Error::protocol);
          if (auto result = ready(message); !result)
            co_await fail(result.error());

          delivery.result.transaction = transaction;
          aborted = false;
          completed = true;
          {
            std::lock_guard lock(pipeline_state.mutex);
            pipeline_state.unsynchronized = entry.id != pipeline_state.submitted_sequence;
          }
          break;
        }
      }

      {
        std::lock_guard lock(pipeline_state.mutex);
        pipeline_state.aborted = aborted;
      }
      co_await buffer.publish();
    }
  }

  Task<CopyFormat> start_copy(std::string sql, [[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (copy != CopyMode::none)
      co_await fail(Error::busy);
    if (!detail::cstring_valid(sql))
      co_await fail(std::errc::invalid_argument);
    if (sql.size() > options.limits.message_bytes - 1)
      co_await fail(Error::resource_limit);

    detail::Writer body;
    body.string(sql);
    detail::Writer request;
    request.message('Q', body);
    if (request.bytes.size() > options.limits.message_bytes)
      co_await fail(Error::resource_limit);

    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    error = {};
    operation_error = {};
    copied.reset();
    copy_send_done = false;
    copy_receive_done = false;
    co_await write(request.bytes);
    std::error_code failure;
    for (;;) {
      auto message = co_await receive();
      auto handled = administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (*handled)
        continue;

      if (message.kind == 'E') {
        auto diagnostic = detail::diagnostic(message.body);
        if (!diagnostic)
          co_await fail(diagnostic.error());

        error = std::move(*diagnostic);
        failure = sql_error(error.sqlstate());
        if (failure == make_error_code(Error::protocol))
          co_await fail(Error::protocol);
      } else if (message.kind == 'Z' && failure) {
        if (auto result = ready(message); !result)
          co_await fail(result.error());

        guard.completed = true;
        co_await fail(failure);
      } else if (!failure && (message.kind == 'G' || message.kind == 'H' || message.kind == 'W')) {
        auto format = copy_format(message);
        if (!format)
          co_await fail(format.error());
        if (copy_generation == std::numeric_limits<u64>::max())
          co_await fail(Error::resource_limit);
        ++copy_generation;
        if (message.kind == 'G') {
          copy = CopyMode::input;
        } else if (message.kind == 'H') {
          copy = CopyMode::output;
        } else {
          copy = CopyMode::both;
        }
        guard.completed = true;
        co_return std::move(*format);
      } else {
        co_await fail(Error::protocol);
      }
    }
  }

  Task<void> write_copy(std::span<const std::byte> data, u64 generation, [[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (generation != copy_generation || (copy != CopyMode::input && copy != CopyMode::both) || copy_send_done)
      co_await fail(std::errc::invalid_argument);
    if (data.size() > options.limits.message_bytes)
      co_await fail(Error::resource_limit);

    std::optional<Semaphore::Permit> permit;
    if (copy == CopyMode::both) {
      permit.emplace(co_await copy_send.acquire());
      if (generation != copy_generation || copy != CopyMode::both || copy_send_done)
        co_await fail(std::errc::invalid_argument);
    }

    Guard guard{*this};
    guard.sender = copy == CopyMode::both;
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    detail::Writer body;
    body.raw(data);
    detail::Writer request;
    request.message('d', body);
    co_await write(request.bytes);
    guard.completed = true;
  }

  Task<void> finish_copy_send(u64 generation, [[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (generation != copy_generation || copy != CopyMode::both)
      co_await fail(std::errc::invalid_argument);

    auto permit = co_await copy_send.acquire();
    if (generation != copy_generation || copy != CopyMode::both)
      co_await fail(std::errc::invalid_argument);
    Guard guard{*this};
    guard.sender = true;
    if (auto status = guard.acquire(); !status)
      co_await fail(status.error());
    co_await guard.capture();
    if (copy_send_done) {
      guard.completed = true;
      co_return;
    }

    detail::Writer request;
    request.message('c');
    // A peer can finish receiving before the local send completion is dispatched.
    copy_send_done = true;
    co_await write(request.bytes);
    guard.completed = true;
  }

  Task<std::optional<std::vector<std::byte>>> copy_response(bool output)
  {
    bool done = !output;
    const bool duplex = copy == CopyMode::both;
    std::error_code failure;
    ResultSet result;
    bool command = false;
    Results results;
    bool description = false;
    std::size_t retained = 0;
    for (;;) {
      auto message = co_await receive();
      auto handled = administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (*handled)
        continue;

      if (message.kind == 'E') {
        auto diagnostic = detail::diagnostic(message.body);
        if (!diagnostic)
          co_await fail(diagnostic.error());

        error = std::move(*diagnostic);
        failure = sql_error(error.sqlstate());
        if (failure == make_error_code(Error::protocol))
          co_await fail(Error::protocol);
        if (duplex)
          co_await fail(failure);
        done = true;
      } else if (!done && message.kind == 'd') {
        co_return std::move(message.body);
      } else if (!done && message.kind == 'c' && message.body.empty()) {
        done = true;
        if (duplex) {
          copy_receive_done = true;
          co_return std::nullopt;
        }
      } else if (duplex && done && options.replication != Replication::disabled && replication_keepalive(message)) {
        // PostgreSQL can enqueue a final keepalive after its CopyDone; never acknowledge it implicitly.
        continue;
      } else if (duplex && done && !failure && message.kind == 'T' && !description && !command && results.empty()) {
        auto columns = detail::columns(message.body, detail::ResultAccess::schema(result));
        if (!columns)
          co_await fail(columns.error());

        result.columns = std::move(*columns);
        retained += message.body.size() + sizeof(ResultSet) + result.columns.size() * sizeof(Column);
        description = true;
      } else if (duplex && done && !failure && message.kind == 'D' && description && !command) {
        auto row = detail::row(message.body, result.columns, detail::ResultAccess::rows(result));
        if (!row)
          co_await fail(row.error());

        retained += message.body.size() + sizeof(Row) + row->size() * sizeof(Value);
        result.rows.push_back(std::move(*row));
      } else if (done && !failure && message.kind == 'C' && (!command || (duplex && results.size() < 2))) {
        detail::Reader reader{message.body};
        result.command = reader.string();
        if (!reader.empty())
          co_await fail(Error::protocol);

        command = true;
        retained += message.body.size() + sizeof(ResultSet);
        result.kind = description ? ResultKind::tuples : ResultKind::command;
        observe_result(result);
        results.push_back(std::move(result));
        result = {};
        description = false;
      } else if (done && message.kind == 'Z' && (command || failure)) {
        if (auto status = ready(message); !status)
          co_await fail(status.error());

        copy = CopyMode::none;
        if (results.empty() && failure)
          results.emplace_back();
        copied = std::move(results);
        if (failure)
          co_await fail(failure);

        co_return std::nullopt;
      } else {
        co_await fail(Error::protocol);
      }
      if (retained > options.limits.result_bytes)
        co_await fail(Error::resource_limit);
    }
  }

  Task<std::optional<std::vector<std::byte>>> read_copy(u64 generation, [[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (generation != copy_generation || (copy != CopyMode::output && copy != CopyMode::both))
      co_await fail(std::errc::invalid_argument);

    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    if (copy == CopyMode::both && copy_receive_done) {
      guard.completed = true;
      co_return std::nullopt;
    }

    auto result = co_await as_result(copy_response(true));
    guard.completed = result.has_value() || copy == CopyMode::none;
    if (!result)
      co_await fail(result.error());

    co_return std::move(*result);
  }

  Task<ResultSet> end_copy(std::optional<std::string> failure, u64 generation, [[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (generation != copy_generation || (copy != CopyMode::input && copy != CopyMode::both) ||
      (failure && !detail::cstring_valid(*failure)))
      co_await fail(std::errc::invalid_argument);
    if (copy == CopyMode::both && failure)
      co_await fail(std::errc::operation_not_supported);
    if (failure && failure->size() > options.limits.message_bytes - 1)
      co_await fail(Error::resource_limit);

    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    if (copy == CopyMode::both) {
      co_await finish_copy_send(generation);
      while (!copy_receive_done)
        static_cast<void>(co_await copy_response(true));
    } else {
      detail::Writer request;
      if (failure) {
        detail::Writer body;
        body.string(*failure);
        request.message('f', body);
      } else {
        request.message('c');
      }

      co_await write(request.bytes);
    }

    auto result = co_await as_result(copy_response(false));
    guard.completed = copy == CopyMode::none;
    if (!result)
      co_await fail(result.error());

    co_return ResultSet{copied->back()};
  }

  Task<void> start_rows(std::string sql, [[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (copy != CopyMode::none)
      co_await fail(Error::busy);
    if (!detail::cstring_valid(sql) || sql.size() > options.limits.message_bytes - 1)
      co_await fail(std::errc::invalid_argument);

    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    detail::Writer body;
    body.string(sql);
    detail::Writer request;
    request.message('Q', body);
    co_await write(request.bytes);
    copy = CopyMode::rows;
    streaming_columns.clear();
    streaming_description = false;
    error = {};
    operation_error = {};
    guard.completed = true;
  }

  Task<std::optional<Row>> read_row([[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (copy != CopyMode::rows)
      co_await fail(std::errc::invalid_argument);

    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    std::error_code failure;
    for (;;) {
      auto message = co_await receive();
      auto handled = administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (*handled)
        continue;

      if (message.kind == 'E') {
        auto diagnostic = detail::diagnostic(message.body);
        if (!diagnostic)
          co_await fail(diagnostic.error());

        error = std::move(*diagnostic);
        failure = sql_error(error.sqlstate());
        if (failure == make_error_code(Error::protocol))
          co_await fail(Error::protocol);
      } else if (message.kind == 'Z') {
        if (streaming_description && !failure)
          co_await fail(Error::protocol);
        if (auto result = ready(message); !result)
          co_await fail(result.error());

        copy = CopyMode::none;
        guard.completed = true;
        if (failure)
          co_await fail(failure);

        co_return std::nullopt;
      } else if (!failure && message.kind == 'T') {
        if (streaming_description)
          co_await fail(Error::protocol);
        auto columns = detail::columns(message.body);
        if (!columns)
          co_await fail(columns.error());

        streaming_columns = std::move(*columns);
        streaming_description = true;
      } else if (!failure && message.kind == 'D') {
        if (!streaming_description)
          co_await fail(Error::protocol);
        auto row = detail::row(message.body, streaming_columns);
        if (!row)
          co_await fail(row.error());

        guard.completed = true;
        co_return std::move(*row);
      } else if (!failure && message.kind == 'C') {
        detail::Reader reader{message.body};
        reader.string();
        if (!reader.empty())
          co_await fail(Error::protocol);

        streaming_description = false;
      } else if (!failure && message.kind == 'I' && message.body.empty()) {
        // Empty statements produce no rows.
      } else {
        co_await fail(Error::protocol);
      }
    }
  }

  Task<ResultSet> open_portal(
    std::string name,
    std::string sql,
    std::vector<Parameter> parameters,
    [[maybe_unused]] Borrow borrow = {})
  {
    if (options.replication != Replication::disabled)
      co_await fail(std::errc::operation_not_supported);
    if (transaction != Transaction::active || name.empty() || !detail::cstring_valid(name))
      co_await fail(std::errc::invalid_argument);
    if (name.size() > options.limits.message_bytes / 2 || sql.size() > options.limits.message_bytes / 2 ||
      parameters.size() > 65535)
      co_await fail(Error::resource_limit);

    std::vector<u32> types;
    for (const auto &parameter : parameters)
      types.push_back(parameter.type);

    auto request = parse("", sql, types);
    if (!request)
      co_await fail(request.error());
    if (auto result = detail::bind(*request, "", parameters, Format::text, options.limits.message_bytes, name); !result)
      co_await fail(result.error());

    detail::Writer description;
    description.integer('P', 1);
    description.string(name);
    request->message('D', description);
    request->message('S');
    auto results = co_await exchange(std::move(*request), 1, ExchangeKind::description);
    if (results.size() != 1)
      co_await fail(Error::protocol);

    co_return std::move(results.front());
  }

  Task<ResultSet> fetch(std::string name, u32 rows, [[maybe_unused]] Borrow borrow = {})
  {
    if (options.replication != Replication::disabled)
      co_await fail(std::errc::operation_not_supported);
    if (transaction != Transaction::active || name.empty() || !rows || !detail::cstring_valid(name))
      co_await fail(std::errc::invalid_argument);
    if (name.size() > options.limits.message_bytes / 2)
      co_await fail(Error::resource_limit);

    detail::Writer description;
    description.integer('P', 1);
    description.string(name);
    detail::Writer request;
    request.message('D', description);
    detail::Writer execution;
    execution.string(name);
    execution.integer(rows);
    request.message('E', execution);
    request.message('S');
    auto results = co_await exchange(std::move(request), 1, ExchangeKind::fetch);
    if (results.size() != 1)
      co_await fail(Error::protocol);

    co_return std::move(results.front());
  }

  Task<void> close_portal(std::string name, [[maybe_unused]] Borrow borrow = {})
  {
    if (!detail::cstring_valid(name) || name.size() > options.limits.message_bytes - 8)
      co_await fail(std::errc::invalid_argument);

    detail::Writer body;
    body.integer('P', 1);
    body.string(name);
    detail::Writer request;
    request.message('C', body);
    request.message('S');
    co_await exchange(std::move(request), 0);
  }

  Task<Notification> wait_notification([[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (copy != CopyMode::none)
      co_await fail(Error::busy);

    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    while (notifications.empty()) {
      auto message = co_await receive();
      if (message.kind == 'A') {
        auto value = detail::notification(message.body);
        if (!value)
          co_await fail(value.error());
        if (notification_receiver)
          notification_receiver->deliver(*value);

        guard.completed = true;
        co_return std::move(*value);
      }
      auto handled = administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (!*handled)
        co_await fail(Error::protocol);
    }

    auto value = std::move(notifications.front());
    notification_bytes -= value.channel.size() + value.payload.size() + 6;
    notifications.erase(notifications.begin());
    guard.completed = true;
    co_return value;
  }

  Task<void> finish([[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (copy == CopyMode::pipeline || copy == CopyMode::both || copy == CopyMode::exchange)
      co_await fail(Error::busy);
    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    detail::Writer request;
    request.message('X');
    co_await write(request.bytes);
    if (auto *secured = std::get_if<TlsStream<TcpStream>>(&transport))
      co_await secured->shutdown_send();
    if (auto *secured = std::get_if<detail::GssStream>(&transport))
      co_await secured->finish();

    if (auto result = close_transport(); !result)
      co_await fail(result.error());

    guard.completed = true;
  }

  Task<Value> call_function(
    u32 function,
    std::vector<Parameter> parameters,
    Format format,
    [[maybe_unused]] Borrow borrow = {})
  {
    co_await cancellation_point();
    if (copy != CopyMode::none)
      co_await fail(Error::busy);
    if (options.replication != Replication::disabled)
      co_await fail(std::errc::operation_not_supported);

    auto request = detail::function_call(function, parameters, format, options.limits.message_bytes);
    if (!request)
      co_await fail(request.error());

    Guard guard{*this};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    error = {};
    operation_error = {};
    co_await write(request->bytes);
    std::optional<Value> value;
    std::error_code failure;
    for (;;) {
      auto message = co_await receive();
      auto handled = administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (*handled)
        continue;

      if (message.kind == 'V') {
        if (value || failure)
          co_await fail(Error::protocol);

        detail::Reader reader{message.body};
        auto size = reader.integer();
        value.emplace();
        value->format = format;
        if (size != 0xffffffff) {
          auto bytes = reader.take(size);
          if (!reader.valid())
            co_await fail(Error::protocol);

          value->data.emplace(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        }
        if (!reader.empty())
          co_await fail(Error::protocol);
      } else if (message.kind == 'E') {
        if (value || failure)
          co_await fail(Error::protocol);

        auto diagnostic = detail::diagnostic(message.body);
        if (!diagnostic)
          co_await fail(diagnostic.error());

        error = std::move(*diagnostic);
        failure = sql_error(error.sqlstate());
        if (failure == make_error_code(Error::protocol))
          co_await fail(Error::protocol);
      } else if (message.kind == 'Z') {
        if (auto result = ready(message); !result)
          co_await fail(result.error());
        if (!value && !failure)
          co_await fail(Error::protocol);

        guard.completed = true;
        if (failure)
          co_await fail(failure);

        co_return std::move(*value);
      } else {
        co_await fail(Error::protocol);
      }
    }
  }
};

struct Exchange::Impl {
  Connection::Impl *connection;
  std::optional<Connection::Borrow> lease;
  std::atomic<std::size_t> borrowers{0};
  Semaphore send_gate{1};
  std::optional<CopyDirection> direction;
  u64 generation = 0;
  bool send_done = false;
  bool receive_done = false;
  bool description = false;
  bool command_seen = false;
  bool completed = false;
  u32 chunk_rows = 0;
  ResultSet result;
  std::optional<detail::Message> pending;
  std::size_t retained = 0;
  std::size_t schema_bytes = 0;

  struct Lease {
    Impl *owner;
    Connection::Impl *connection;
    u64 generation;

    explicit Lease(Impl &state) noexcept : owner(&state), connection(state.connection), generation(state.generation)
    {
      owner->borrowers.fetch_add(1, std::memory_order_relaxed);
      if (connection)
        connection->borrowers.fetch_add(1, std::memory_order_relaxed);
    }

    Lease(Lease &&other) noexcept
        : owner(std::exchange(other.owner, nullptr)), connection(other.connection), generation(other.generation)
    {
    }

    Lease(const Lease &) = delete;

    ~Lease()
    {
      if (!owner)
        return;
      if (connection) {
        auto count = connection->borrowers.fetch_sub(1, std::memory_order_acq_rel);
        weave::detail::require(count != 0);
      }
      auto count = owner->borrowers.fetch_sub(1, std::memory_order_acq_rel);
      weave::detail::require(count != 0);
    }
  };

  Impl(Connection &owner, RowOptions rows)
      : connection(&owner.state()), lease(std::in_place, owner), chunk_rows(rows.chunk_rows)
  {
  }

  ~Impl()
  {
    weave::detail::require(borrowers.load(std::memory_order_acquire) == 0);
    if (!connection)
      return;
    if (!completed)
      static_cast<void>(connection->close_transport());
    connection->copy = CopyMode::none;
  }

  Result<void> charge(std::size_t bytes)
  {
    if (bytes > connection->options.limits.result_bytes - retained)
      return std::unexpected(make_error_code(Error::resource_limit));
    retained += bytes;
    return {};
  }

  ExchangeEvent publish_chunk()
  {
    ResultSet chunk;
    chunk.columns = result.columns;
    chunk.rows = std::move(result.rows);
    result.rows = detail::ResultList<Row>{};
    chunk.kind = ResultKind::row_chunk;
    connection->observe_result(chunk);
    // Reserve both the staging schema and the next owning chunk's schema.
    retained = schema_bytes * 2;
    return ExchangeEvent{std::move(chunk)};
  }

  Task<std::optional<ExchangeEvent>> next([[maybe_unused]] Lease borrow)
  {
    co_await cancellation_point();
    if (completed)
      co_return std::nullopt;
    if (!connection)
      co_await fail(Error::closed);

    Connection::Impl::Guard guard{*connection};
    if (auto status = guard.acquire(); !status)
      co_await fail(status.error());
    co_await guard.capture();

    std::error_code failure;
    for (;;) {
      auto message = pending ? std::move(*pending) : co_await connection->receive();
      pending.reset();
      auto handled = connection->administrative(message);
      if (!handled)
        co_await fail(handled.error());
      if (*handled)
        continue;

      bool copy_tail = direction && send_done && receive_done;
      bool copy_start = message.kind == 'G' || message.kind == 'H' || message.kind == 'W';
      bool replication_boundary = connection->options.replication != Replication::disabled &&
        (message.kind == 'T' || copy_start);
      if (!failure && replication_boundary && description && (!direction || copy_tail)) {
        if (auto status = charge(message.body.size()); !status)
          co_await fail(status.error());
        pending.emplace(std::move(message));
        result.kind = ResultKind::row_chunk;
        connection->observe_result(result);
        auto event = ExchangeEvent{std::move(result)};
        result = {};
        retained = 0;
        schema_bytes = 0;
        description = false;
        guard.completed = true;
        co_return event;
      }
      if (message.kind == 'E' && !failure) {
        auto diagnostic = detail::diagnostic(message.body);
        if (!diagnostic)
          co_await fail(diagnostic.error());
        connection->error = std::move(*diagnostic);
        failure = sql_error(connection->error.sqlstate());
        if (failure == make_error_code(Error::protocol))
          co_await fail(failure);
        // A bidirectional sender may still be active or deferred. Do not race it with recovery.
        if (direction && *direction == CopyDirection::both)
          co_await fail(failure);
        if (direction && *direction == CopyDirection::input &&
          (connection->copy_writing.load(std::memory_order_acquire) || borrowers.load(std::memory_order_acquire) != 1))
          co_await fail(failure);
        result = {};
        retained = 0;
        schema_bytes = 0;
        description = false;
        direction.reset();
      } else if (message.kind == 'Z') {
        if (description || direction || (!command_seen && !failure))
          co_await fail(Error::protocol);
        if (auto status = connection->ready(message); !status)
          co_await fail(status.error());
        completed = true;
        guard.completed = true;
        if (failure)
          co_await fail(failure);
        co_return std::nullopt;
      } else if (failure) {
        co_await fail(Error::protocol);
      } else if (message.kind == 'T' && !description && (!direction || copy_tail)) {
        if (copy_tail)
          direction.reset();
        auto columns = detail::columns(message.body, detail::ResultAccess::schema(result));
        if (!columns)
          co_await fail(columns.error());
        schema_bytes = message.body.size() + sizeof(ResultSet) + columns->size() * sizeof(Column);
        if (auto status = charge(schema_bytes); !status)
          co_await fail(status.error());
        if (chunk_rows) {
          if (auto status = charge(schema_bytes); !status)
            co_await fail(status.error());
        }
        result.columns = std::move(*columns);
        description = true;
      } else if (message.kind == 'D' && description && (!direction || copy_tail)) {
        auto row = detail::row(message.body, result.columns, detail::ResultAccess::rows(result));
        if (!row)
          co_await fail(row.error());
        auto bytes = message.body.size() + sizeof(Row) + row->size() * sizeof(Value);
        bool full = bytes > connection->options.limits.result_bytes - retained;
        if (chunk_rows && full && !result.rows.empty()) {
          pending.emplace(std::move(message));
          auto event = publish_chunk();
          guard.completed = true;
          co_return event;
        }
        if (auto status = charge(bytes); !status)
          co_await fail(status.error());
        result.rows.push_back(std::move(*row));
        if (chunk_rows && result.rows.size() >= chunk_rows) {
          auto event = publish_chunk();
          guard.completed = true;
          co_return event;
        }
      } else if (copy_start) {
        if (copy_tail && connection->options.replication != Replication::disabled)
          direction.reset();
        if (description || direction)
          co_await fail(Error::protocol);
        auto format = copy_format(message);
        if (!format)
          co_await fail(format.error());
        if (auto status = charge(message.body.size() + sizeof(CopyFormat) + format->columns.size() * sizeof(Format));
          !status)
          co_await fail(status.error());
        direction = format->direction;
        send_done = *direction == CopyDirection::output;
        receive_done = *direction == CopyDirection::input;
        if (generation == std::numeric_limits<u64>::max())
          co_await fail(Error::resource_limit);
        ++generation;
        retained = 0;
        guard.completed = true;
        co_return ExchangeEvent{std::move(*format)};
      } else if (direction == CopyDirection::both && receive_done &&
        connection->options.replication != Replication::disabled && replication_keepalive(message)) {
        continue;
      } else if (message.kind == 'd' && direction && *direction != CopyDirection::input && !receive_done) {
        guard.completed = true;
        co_return ExchangeEvent{std::move(message.body)};
      } else if (message.kind == 'c' && message.body.empty() && direction && *direction != CopyDirection::input &&
        !receive_done) {
        receive_done = true;
        guard.completed = true;
        co_return ExchangeEvent{CopyDone{}};
      } else if (message.kind == 'C') {
        if (direction && (!receive_done || !send_done))
          co_await fail(Error::protocol);
        if (chunk_rows && description && !result.rows.empty()) {
          pending.emplace(std::move(message));
          auto event = publish_chunk();
          guard.completed = true;
          co_return event;
        }
        detail::Reader reader{message.body};
        result.command = reader.string();
        if (!reader.empty() || result.command.empty())
          co_await fail(Error::protocol);
        if (auto status = charge(message.body.size()); !status)
          co_await fail(status.error());
        result.kind = description ? ResultKind::tuples : ResultKind::command;
        connection->observe_result(result);
        auto event = ExchangeEvent{std::move(result)};
        result = {};
        retained = 0;
        schema_bytes = 0;
        description = false;
        direction.reset();
        command_seen = true;
        guard.completed = true;
        co_return event;
      } else if (message.kind == 'I' && message.body.empty() && !description && !direction) {
        command_seen = true;
        guard.completed = true;
        ResultSet empty;
        empty.kind = ResultKind::empty_query;
        connection->observe_result(empty);
        co_return ExchangeEvent{std::move(empty)};
      } else {
        co_await fail(Error::protocol);
      }
    }
  }

  Task<void> send(std::span<const std::byte> data, bool finish, std::optional<std::string> error, Lease borrow)
  {
    co_await cancellation_point();
    if (completed || !connection)
      co_await fail(Error::closed);
    if (data.size() > connection->options.limits.message_bytes ||
      (error && error->size() > connection->options.limits.message_bytes - 1))
      co_await fail(Error::resource_limit);
    if (error && !detail::cstring_valid(*error))
      co_await fail(std::errc::invalid_argument);

    auto permit = co_await send_gate.acquire();
    if (borrow.generation != generation || !direction || *direction == CopyDirection::output)
      co_await fail(std::errc::invalid_argument);
    if (error && *direction == CopyDirection::both)
      co_await fail(std::errc::operation_not_supported);
    if (send_done) {
      if (finish && !error)
        co_return;
      co_await fail(std::errc::invalid_argument);
    }

    Connection::Impl::Guard guard{*connection};
    guard.sender = true;
    if (auto status = guard.acquire(); !status)
      co_await fail(status.error());
    co_await guard.capture();

    detail::Writer body;
    detail::Writer request;
    if (finish) {
      if (error) {
        body.string(*error);
        request.message('f', body);
      } else {
        request.message('c');
      }
      // The backend can respond before the local send completion is dispatched.
      send_done = true;
    } else {
      body.raw(data);
      request.message('d', body);
    }
    co_await connection->write(request.bytes);
    guard.completed = true;
  }
};

Exchange::Exchange(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

Exchange::Exchange(Exchange &&other) noexcept = default;
Exchange::~Exchange() = default;

Exchange::Impl &Exchange::state() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return *impl_;
}

Task<std::optional<ExchangeEvent>> Exchange::next()
{
  return state().next(Impl::Lease{state()});
}

Task<void> Exchange::write(std::span<const std::byte> data)
{
  return state().send(data, false, std::nullopt, Impl::Lease{state()});
}

Task<void> Exchange::finish_send(std::optional<std::string> error)
{
  return state().send({}, true, std::move(error), Impl::Lease{state()});
}

Result<void> Exchange::finish()
{
  auto &impl = state();
  if (!impl.connection)
    return {};
  if (impl.borrowers.load(std::memory_order_acquire) != 0)
    return std::unexpected(make_error_code(Error::busy));
  if (!impl.completed)
    return std::unexpected(make_error_code(impl.connection->connected ? Error::busy : Error::closed));
  impl.connection->copy = CopyMode::none;
  impl.connection = nullptr;
  impl.lease.reset();
  return {};
}

Task<Exchange> Connection::begin_exchange(std::string sql, RowOptions rows, [[maybe_unused]] Borrow borrow)
{
  co_await cancellation_point();
  auto &connection = state();
  if (connection.copy != CopyMode::none)
    co_await fail(Error::busy);
  if (!detail::cstring_valid(sql))
    co_await fail(std::errc::invalid_argument);
  if (sql.size() > connection.options.limits.message_bytes - 1)
    co_await fail(Error::resource_limit);

  detail::Writer body;
  body.string(sql);
  detail::Writer request;
  request.message('Q', body);
  if (request.bytes.size() > connection.options.limits.message_bytes)
    co_await fail(Error::resource_limit);

  Connection::Impl::Guard guard{connection};
  if (auto status = guard.acquire(); !status)
    co_await fail(status.error());
  co_await guard.capture();
  auto exchange = std::make_unique<Exchange::Impl>(*this, rows);
  connection.copy = CopyMode::exchange;
  connection.error = {};
  connection.operation_error = {};
  connection.copied.reset();
  co_await connection.write(request.bytes);
  guard.completed = true;
  co_return Exchange{std::move(exchange)};
}

Task<Exchange> Connection::exchange(std::string sql, RowOptions rows)
{
  return begin_exchange(std::move(sql), rows, Borrow{*this});
}

struct Pipeline::Impl {
  Connection::Impl *connection;
  std::optional<Connection::Borrow> lease;
  PipelineState pipeline;

  explicit Impl(Connection &owner)
      : connection(&owner.state()), lease(std::in_place, owner), pipeline(owner.state().options.limits)
  {
    pipeline.events = connection->events.get();
  }

  ~Impl()
  {
    weave::detail::require(
      pipeline.borrowers.load(std::memory_order_acquire) == 0 && !pipeline.flushing && !pipeline.sending &&
      !pipeline.reading);
    if (!connection)
      return;

    // Dropping an unsynchronized exchange cannot leave a reusable wire session.
    if (pipeline.unsynchronized || pipeline.aborted || !pipeline.outbound.bytes.empty() || !pipeline.submitted.empty())
      static_cast<void>(connection->close_transport());

    connection->pipeline = nullptr;
    connection->copy = CopyMode::none;
  }

  Task<void> flush([[maybe_unused]] Pipeline::Borrow borrow)
  {
    co_await cancellation_point();
    detail::Writer request;
    std::vector<PipelineEntry> entries;
    std::error_code failure;
    {
      std::lock_guard lock(pipeline.mutex);
      if (pipeline.terminal)
        failure = pipeline.failure ? pipeline.failure : make_error_code(Error::closed);
      else if (pipeline.flushing || pipeline.sending || pipeline.reading || !pipeline.outbound.bytes.empty() ||
        !pipeline.submitted.empty())
        failure = make_error_code(Error::busy);
      else if (!pipeline.request.bytes.empty()) {
        pipeline.flushing = true;
        request = std::move(pipeline.request);
        entries = std::move(pipeline.pending);
        if (!entries.empty()) {
          pipeline.unsynchronized = true;
          pipeline.submitted_sequence = entries.back().id;
        }
        pipeline.request = {};
        pipeline.pending.clear();
      }
    }

    if (failure)
      co_await fail(failure);
    if (request.bytes.empty())
      co_return;

    struct Cleanup {
      PipelineState &pipeline;

      ~Cleanup()
      {
        std::lock_guard lock(pipeline.mutex);
        pipeline.flushing = false;
      }
    } cleanup{pipeline};

    Connection::Impl::Guard guard{*connection};
    if (auto result = guard.acquire(); !result)
      co_await fail(result.error());
    co_await guard.capture();

    // Backend Flush requests responses without implicitly committing via Sync.
    request.message('H');
    auto cancel_peer = [this](weave::Error error) noexcept {
      pipeline.stop(error);
      connection->connected = false;
      static_cast<void>(std::visit(
        [](auto &stream) {
          return stream.cancel();
        },
        connection->transport));
    };

    auto completed = co_await as_result(when_all(
      connection->write(request.bytes).on_error(cancel_peer),
      connection->receive_pipeline(pipeline, std::move(entries)).on_error(cancel_peer)));
    if (!completed) {
      {
        std::lock_guard lock(pipeline.mutex);
        failure = pipeline.failure;
      }
      if (!failure)
        failure = completed.error();

      pipeline.stop(failure);
      co_await fail(failure);
    }
    guard.completed = true;
  }

  // Called under the pipeline lock; either direction may reserve the next batch.
  void submit()
  {
    if (!pipeline.outbound.bytes.empty() || pipeline.request.bytes.empty())
      return;

    pipeline.outbound = std::move(pipeline.request);
    pipeline.outbound.message('H');
    pipeline.request = {};
    if (!pipeline.pending.empty()) {
      pipeline.submitted_sequence = pipeline.pending.back().id;
      pipeline.unsynchronized = true;
    }
    for (auto &entry : pipeline.pending)
      pipeline.submitted.push_back(std::move(entry));
    pipeline.pending.clear();
  }

  std::error_code stop_io(std::error_code error) noexcept
  {
    pipeline.stop(error);
    connection->connected = false;
    static_cast<void>(std::visit(
      [](auto &stream) {
        return stream.cancel();
      },
      connection->transport));

    std::lock_guard lock(pipeline.mutex);
    return pipeline.failure;
  }

  Task<void> send([[maybe_unused]] Pipeline::Borrow borrow)
  {
    co_await cancellation_point();
    detail::Writer request;
    std::error_code failure;
    {
      std::lock_guard lock(pipeline.mutex);
      if (pipeline.terminal)
        failure = pipeline.failure ? pipeline.failure : make_error_code(Error::closed);
      else if (pipeline.flushing || pipeline.sending)
        failure = make_error_code(Error::busy);
      else {
        submit();
        if (!pipeline.outbound.bytes.empty()) {
          pipeline.sending = true;
          request = std::move(pipeline.outbound);
          pipeline.outbound = {};
        }
      }
    }
    if (failure)
      co_await fail(failure);
    if (request.bytes.empty())
      co_return;

    struct Cleanup {
      PipelineState &pipeline;

      ~Cleanup()
      {
        std::lock_guard lock(pipeline.mutex);
        pipeline.sending = false;
      }
    } cleanup{pipeline};

    Connection::Impl::Guard guard{*connection};
    guard.sender = true;
    if (auto acquired = guard.acquire(); !acquired)
      co_await fail(stop_io(acquired.error()));
    co_await guard.capture();

    auto written = co_await as_result(connection->write(request.bytes));
    if (!written)
      co_await fail(stop_io(written.error()));
    guard.completed = true;
  }

  Task<void> receive([[maybe_unused]] Pipeline::Borrow borrow)
  {
    co_await cancellation_point();
    std::vector<PipelineEntry> entries;
    std::error_code failure;
    {
      std::lock_guard lock(pipeline.mutex);
      if (pipeline.terminal)
        failure = pipeline.failure ? pipeline.failure : make_error_code(Error::closed);
      else if (pipeline.flushing || pipeline.reading)
        failure = make_error_code(Error::busy);
      else {
        if (pipeline.submitted.empty() && !pipeline.sending)
          submit();
        if (!pipeline.submitted.empty()) {
          pipeline.reading = true;
          entries = std::move(pipeline.submitted);
          pipeline.submitted.clear();
        }
      }
    }
    if (failure)
      co_await fail(failure);
    if (entries.empty())
      co_return;

    struct Cleanup {
      PipelineState &pipeline;

      ~Cleanup()
      {
        std::lock_guard lock(pipeline.mutex);
        pipeline.reading = false;
      }
    } cleanup{pipeline};

    Connection::Impl::Guard guard{*connection};
    if (auto acquired = guard.acquire(); !acquired)
      co_await fail(stop_io(acquired.error()));
    co_await guard.capture();

    auto read = co_await as_result(connection->receive_pipeline(pipeline, std::move(entries)));
    if (!read)
      co_await fail(stop_io(read.error()));
    guard.completed = true;
  }

  Task<std::optional<PipelineResult>> next([[maybe_unused]] Pipeline::Borrow borrow)
  {
    co_await cancellation_point();
    std::error_code failure;
    bool empty = false;
    {
      std::lock_guard lock(pipeline.mutex);
      if (pipeline.receiving)
        failure = make_error_code(Error::busy);
      else if (!pipeline.outstanding && !pipeline.failure)
        empty = true;
      else
        pipeline.receiving = true;
    }
    if (failure)
      co_await fail(failure);
    if (empty)
      co_return std::nullopt;

    struct Cleanup {
      PipelineState &pipeline;

      ~Cleanup()
      {
        std::lock_guard lock(pipeline.mutex);
        pipeline.receiving = false;
      }
    } cleanup{pipeline};

    auto delivery = co_await pipeline.results.receive();
    if (delivery) {
      auto result = pipeline.consume(std::move(*delivery));
      if (!result)
        co_await fail(result.error());

      co_return std::move(*result);
    }

    {
      std::lock_guard lock(pipeline.mutex);
      failure = pipeline.failure;
    }
    if (failure)
      co_await fail(failure);

    co_return std::nullopt;
  }

  Result<u64> execute(std::string sql, std::vector<Parameter> parameters, Format format, bool prepared, RowOptions rows)
  {
    if (sql.size() > pipeline.limits.message_bytes - 16 || parameters.size() > 65535)
      return std::unexpected(make_error_code(Error::resource_limit));

    detail::Writer request;
    if (!prepared) {
      std::vector<u32> types;
      for (const auto &parameter : parameters)
        types.push_back(parameter.type);

      auto parsed = parse("", sql, types);
      if (!parsed)
        return std::unexpected(parsed.error());
      request = std::move(*parsed);
    }

    auto bound = detail::bind(
      request,
      prepared ? std::string_view{sql} : std::string_view{},
      parameters,
      format,
      pipeline.limits.message_bytes);
    if (!bound)
      return std::unexpected(bound.error());

    execution(request);
    request.bytes.resize(request.bytes.size() - 5);
    auto step = prepared ? PipelineStep::bind : PipelineStep::parse;
    return pipeline.enqueue(std::move(request), PipelineKind::execute, step, rows);
  }

  Result<u64> named(std::string name, char message, bool portal)
  {
    if (!detail::cstring_valid(name))
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    if (name.size() > pipeline.limits.message_bytes - 8)
      return std::unexpected(make_error_code(Error::resource_limit));

    detail::Writer body;
    body.integer(portal ? 'P' : 'S', 1);
    body.string(name);
    detail::Writer request;
    request.message(message, body);

    auto kind = message == 'D' ? PipelineKind::describe : PipelineKind::close;
    auto step = message == 'D' ? (portal ? PipelineStep::description : PipelineStep::parameters) : PipelineStep::close;
    return pipeline.enqueue(std::move(request), kind, step);
  }
};

Pipeline::Pipeline(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

Pipeline::Pipeline(Pipeline &&other) noexcept
{
  weave::detail::require(!other.impl_ || other.impl_->pipeline.borrowers.load(std::memory_order_acquire) == 0);
  impl_ = std::move(other.impl_);
}

Pipeline::~Pipeline() = default;

Pipeline::Impl &Pipeline::state() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return *impl_;
}

Pipeline::Borrow::Borrow(Impl &owner) noexcept : impl(&owner)
{
  impl->pipeline.borrowers.fetch_add(1, std::memory_order_relaxed);
}

Pipeline::Borrow::Borrow(Borrow &&other) noexcept : impl(std::exchange(other.impl, nullptr))
{
}

Pipeline::Borrow::~Borrow()
{
  if (impl) {
    auto count = impl->pipeline.borrowers.fetch_sub(1, std::memory_order_acq_rel);
    weave::detail::require(count != 0);
  }
}

Result<u64> Pipeline::execute(Command command, RowOptions rows)
{
  return state().execute(std::move(command.sql), std::move(command.parameters), command.format, false, rows);
}

Result<u64> Pipeline::execute_prepared(
  std::string name,
  std::vector<Parameter> parameters,
  Format format,
  RowOptions rows)
{
  return state().execute(std::move(name), std::move(parameters), format, true, rows);
}

Result<u64> Pipeline::prepare(std::string name, std::string sql, std::vector<u32> types)
{
  auto &pipeline = state().pipeline;
  if (name.size() > pipeline.limits.message_bytes / 2 || sql.size() > pipeline.limits.message_bytes / 2 ||
    types.size() > 65535)
    return std::unexpected(make_error_code(Error::resource_limit));

  auto request = parse(name, sql, types);
  if (!request)
    return std::unexpected(request.error());

  return pipeline.enqueue(std::move(*request), PipelineKind::prepare, PipelineStep::parse);
}

Result<u64> Pipeline::describe(std::string name)
{
  return state().named(std::move(name), 'D', false);
}

Result<u64> Pipeline::describe_portal(std::string name)
{
  return state().named(std::move(name), 'D', true);
}

Result<u64> Pipeline::close_prepared(std::string name)
{
  return state().named(std::move(name), 'C', false);
}

Result<u64> Pipeline::close_portal(std::string name)
{
  return state().named(std::move(name), 'C', true);
}

Result<u64> Pipeline::sync()
{
  detail::Writer request;
  request.message('S');
  return state().pipeline.enqueue(std::move(request), PipelineKind::sync, PipelineStep::sync);
}

Result<void> Pipeline::request_flush()
{
  return state().pipeline.request_flush();
}

Task<void> Pipeline::flush()
{
  return state().flush(Borrow{state()});
}

Task<void> Pipeline::send()
{
  return state().send(Borrow{state()});
}

Task<void> Pipeline::receive()
{
  return state().receive(Borrow{state()});
}

Task<std::optional<PipelineResult>> Pipeline::next()
{
  return state().next(Borrow{state()});
}

Result<std::optional<PipelineResult>> Pipeline::try_next()
{
  auto &pipeline = state().pipeline;
  {
    std::lock_guard lock(pipeline.mutex);
    if (pipeline.receiving)
      return std::unexpected(make_error_code(Error::busy));
  }
  auto delivery = pipeline.results.try_receive();
  if (delivery && *delivery) {
    auto result = pipeline.consume(std::move(**delivery));
    if (!result)
      return std::unexpected(result.error());

    return std::optional<PipelineResult>{std::move(*result)};
  }

  std::lock_guard lock(pipeline.mutex);
  if (pipeline.failure)
    return std::unexpected(pipeline.failure);
  if (!pipeline.outstanding)
    return std::optional<PipelineResult>{};

  return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
}

Result<void> Pipeline::finish()
{
  auto &impl = state();
  std::lock_guard lock(impl.pipeline.mutex);
  if (impl.pipeline.detached)
    return {};
  if (impl.pipeline.failure)
    return std::unexpected(impl.pipeline.failure);
  if (impl.pipeline.flushing || impl.pipeline.sending || impl.pipeline.reading ||
    !impl.pipeline.request.bytes.empty() || !impl.pipeline.outbound.bytes.empty() || !impl.pipeline.submitted.empty() ||
    impl.pipeline.outstanding || impl.pipeline.unsynchronized || impl.pipeline.aborted ||
    impl.pipeline.borrowers.load(std::memory_order_acquire) != 0)
    return std::unexpected(make_error_code(Error::busy));

  impl.connection->pipeline = nullptr;
  impl.connection->copy = CopyMode::none;
  impl.connection = nullptr;
  impl.lease.reset();
  impl.pipeline.detached = true;
  impl.pipeline.terminal = true;
  impl.pipeline.results.close();
  impl.pipeline.space.close();
  return {};
}

bool Pipeline::aborted() const noexcept
{
  auto &pipeline = state().pipeline;
  std::lock_guard lock(pipeline.mutex);
  return pipeline.aborted;
}

Result<Pipeline> Connection::pipeline()
{
  auto &connection = state();
  if (!connection.connected)
    return std::unexpected(make_error_code(Error::closed));
  if (connection.busy.load(std::memory_order_acquire) || connection.borrowers.load(std::memory_order_acquire) != 0 ||
    connection.copy != CopyMode::none || (connection.events && connection.events->invoking()))
    return std::unexpected(make_error_code(Error::busy));
  if (connection.options.replication != Replication::disabled)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  auto impl = std::make_unique<Pipeline::Impl>(*this);
  connection.pipeline = &impl->pipeline;
  connection.copy = CopyMode::pipeline;
  return Pipeline{std::move(impl)};
}

Connection::Connection(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

Connection::Connection(Connection &&other) noexcept
{
  weave::detail::require(
    !other.impl_ ||
    (!other.impl_->busy.load(std::memory_order_acquire) &&
      other.impl_->borrowers.load(std::memory_order_acquire) == 0 &&
      (!other.impl_->events || !other.impl_->events->invoking())));
  impl_ = std::move(other.impl_);
  if (impl_ && impl_->events)
    impl_->events->owner = this;
}

Connection::~Connection() = default;

Connection::Impl &Connection::state() const noexcept
{
  weave::detail::require(impl_ != nullptr);
  return *impl_;
}

// Coroutine parameters retain these leases even before their Tasks start.
Connection::Borrow::Borrow(Connection &connection) noexcept : impl(&connection.state())
{
  impl->borrowers.fetch_add(1, std::memory_order_relaxed);
}

Connection::Borrow::Borrow(Borrow &&other) noexcept : impl(std::exchange(other.impl, nullptr))
{
}

Connection::Borrow::~Borrow()
{
  release();
}

void Connection::Borrow::release() noexcept
{
  if (impl) {
    auto count = impl->borrowers.fetch_sub(1, std::memory_order_acq_rel);
    weave::detail::require(count != 0);
    impl = nullptr;
  }
}

Connection::Borrow detail::LargeObjectAccess::borrow(Connection &connection) noexcept
{
  return Connection::Borrow{connection};
}

Task<u32> detail::LargeObjectAccess::resolve(Connection &connection, LargeObjectFunction function)
{
  auto index = static_cast<std::size_t>(function);
  weave::detail::require(index < connection.state().large_object_functions.size());
  if (!connection.state().large_object_functions[index]) {
    auto functions = co_await connection.execute(
      "SELECT signature::pg_catalog.regprocedure::pg_catalog.oid FROM pg_catalog.unnest(ARRAY["
      "'pg_catalog.lo_create(pg_catalog.oid)', 'pg_catalog.lo_open(pg_catalog.oid,pg_catalog.int4)',"
      "'pg_catalog.loread(pg_catalog.int4,pg_catalog.int4)', 'pg_catalog.lowrite(pg_catalog.int4,pg_catalog.bytea)',"
      "'pg_catalog.lo_lseek64(pg_catalog.int4,pg_catalog.int8,pg_catalog.int4)', "
      "'pg_catalog.lo_tell64(pg_catalog.int4)',"
      "'pg_catalog.lo_truncate64(pg_catalog.int4,pg_catalog.int8)', 'pg_catalog.lo_close(pg_catalog.int4)',"
      "'pg_catalog.lo_unlink(pg_catalog.oid)']) WITH ORDINALITY AS f(signature, position) ORDER BY position",
      {},
      Format::binary);
    if (functions.rows.size() != connection.state().large_object_functions.size())
      co_await fail(Error::protocol);

    std::array<u32, static_cast<std::size_t>(LargeObjectFunction::count)> resolved{};
    for (std::size_t offset = 0; offset < resolved.size(); ++offset) {
      const auto &row = functions.rows[offset];
      if (row.size() != 1)
        co_await fail(Error::protocol);
      auto oid = row.front().binary_integer<u32>();
      if (!oid || *oid == 0)
        co_await fail(Error::protocol);
      resolved[offset] = *oid;
    }

    connection.state().large_object_functions = resolved;
  }

  co_return connection.state().large_object_functions[index];
}

std::size_t detail::LargeObjectAccess::chunk_limit(const Connection &connection) noexcept
{
  return connection.state().options.limits.message_bytes - 64;
}

Task<ServerStatus> Connection::probe(detail::OwnedOptions pending)
{
  auto &options = pending.value;
  if (auto valid = valid_options(options); !valid)
    co_await fail(valid.error());
  co_await cancellation_point();

  // A health probe never authenticates or fetches a bearer token. TLS/GSS
  // transport credentials remain necessary for the configured secure channel.
  OPENSSL_cleanse(options.password.data(), options.password.size());
  options.password.clear();
  options.scram_client_key.reset();
  options.scram_server_key.reset();
  options.oauth.reset();
  for (auto &host : options.hosts) {
    if (host.password) {
      OPENSSL_cleanse(host.password->data(), host.password->size());
      host.password.reset();
    }
  }

  auto random = host_randomizer(options.host_balance);
  if (!random)
    co_await fail(random.error());
  if (options.hosts.empty())
    options.hosts.push_back({options.host, options.port, std::nullopt});

  std::array<u8, 64> order;
  auto end = order.begin() + options.hosts.size();
  std::iota(order.begin(), end, u8{0});
  if (*random)
    std::shuffle(order.begin(), end, **random);

  auto status = ServerStatus::no_response;
  for (auto index : std::span{order.begin(), end}) {
    co_await cancellation_point();
    status = ServerStatus::no_response;
    const auto &host = options.hosts[index];
    detail::OwnedOptions candidate{Options{options}};
    candidate.value.host = host.name;
    candidate.value.port = host.port;
    candidate.value.hosts.clear();
    ConnectProgress progress;
    Diagnostic diagnostic;
    auto attempt = co_await as_result(
      Impl::establish(
        std::move(candidate),
        host.address,
        &diagnostic,
        progress,
        TargetSession::any,
        random->get(),
        {},
        {},
        {},
        true));
    co_await cancellation_point();
    if (attempt)
      co_return ServerStatus::accepting;

    auto error = attempt.error();
    if (progress.credentials_failed || progress.direct_tls_pending)
      co_await fail(error);
    if (error == std::errc::operation_canceled || error == std::errc::not_enough_memory ||
      error == std::errc::too_many_files_open || error == std::errc::too_many_files_open_in_system)
      co_await fail(error);

    auto state = diagnostic.sqlstate();
    auto failure_state = sqlstate(error);
    if (!failure_state.empty() && failure_state == state) {
      if (state != "57P03")
        co_return ServerStatus::accepting;
      status = ServerStatus::rejecting;
      continue;
    }

    // A probe never authenticates. Retry unavailable endpoints without changing
    // transport policy, but keep security/protocol and opted-in GSS failures terminal.
    bool plain_timeout = error == std::errc::timed_out && options.gss_encryption == GssEncryption::disable;
    bool disconnected = error == std::errc::connection_reset || error == std::errc::connection_aborted ||
      error == std::errc::broken_pipe;
    bool retry_disconnected = disconnected && options.gss_encryption == GssEncryption::disable;
    if (!progress.transport_connected || plain_timeout || retry_disconnected)
      continue;
    if (disconnected)
      co_return ServerStatus::no_response;
    co_await fail(error);
  }

  co_return status;
}

Task<Connection> Connection::establish(
  detail::OwnedOptions pending,
  Diagnostic *diagnostic,
  std::shared_ptr<NoticeReceiver> notices,
  std::shared_ptr<TraceReceiver> trace,
  std::shared_ptr<NotificationReceiver> notifications,
  ConnectionReport *report)
{
  auto &options = pending.value;
  PasswordCleanup cleanup{options};
  std::unique_ptr<ConnectionHistory> history;
  if (report) {
    history = std::make_unique<ConnectionHistory>(*report);
    history->begin(options.host, options.port);
  }
  if (diagnostic)
    *diagnostic = {};

  if (trace && !valid_trace(trace->configuration)) {
    if (history)
      history->fail(std::make_error_code(std::errc::invalid_argument));
    co_await fail(std::errc::invalid_argument);
  }
  if (auto valid = valid_options(options); !valid) {
    if (history)
      history->fail(valid.error());
    co_await fail(valid.error());
  }

  co_await cancellation_point();
  auto random = host_randomizer(options.host_balance);
  if (!random)
    co_await fail(random.error());

  auto configuration = options.info();
  if (options.hosts.empty() && options.target_session == TargetSession::any) {
    ConnectProgress progress;
    auto impl = co_await Impl::establish(
      std::move(pending),
      std::nullopt,
      diagnostic,
      progress,
      TargetSession::any,
      random->get(),
      notices,
      trace,
      notifications,
      false,
      history ? &*history : nullptr);
    impl->configuration = std::move(configuration);
    co_return Connection{std::move(impl)};
  }

  if (options.hosts.empty())
    options.hosts.push_back({options.host, options.port, std::nullopt});

  std::array<u8, 64> order;
  auto end = order.begin() + options.hosts.size();
  std::iota(order.begin(), end, u8{0});
  if (*random)
    std::shuffle(order.begin(), end, **random);

  bool prefer_standby = options.target_session == TargetSession::prefer_standby;
  int passes = prefer_standby ? 2 : 1;
  std::error_code failure = make_error_code(Error::target_session);
  for (int pass = 0; pass < passes; ++pass) {
    auto target = options.target_session;
    if (prefer_standby)
      target = pass == 0 ? TargetSession::standby : TargetSession::any;

    for (auto index : std::span{order.begin(), end}) {
      co_await cancellation_point();
      const auto &host = options.hosts[index];
      auto candidate = options;
      candidate.host = host.name;
      candidate.port = host.port;
      if (host.password) {
        OPENSSL_cleanse(candidate.password.data(), candidate.password.size());
        candidate.password = *host.password;
      }
      for (auto &destination : candidate.hosts) {
        if (destination.password)
          OPENSSL_cleanse(destination.password->data(), destination.password->size());
      }
      candidate.hosts.clear();
      ConnectProgress progress;
      if (history)
        history->begin(host.name, host.port);
      auto attempt = co_await as_result(
        Impl::establish(
          detail::OwnedOptions{std::move(candidate)},
          host.address,
          diagnostic,
          progress,
          target,
          random->get(),
          notices,
          trace,
          notifications,
          false,
          history ? &*history : nullptr));
      if (attempt) {
        (*attempt)->configuration = std::move(configuration);
        co_return Connection{std::move(*attempt)};
      }

      failure = attempt.error();
      if (progress.credentials_failed || progress.direct_tls_pending || failure == std::errc::operation_canceled)
        co_await fail(failure);
      // Once connected, an opted-in GSS negotiation must not turn a timeout into a weaker host retry.
      bool retry_timeout = failure == std::errc::timed_out && options.gss_encryption == GssEncryption::disable &&
        !options.oauth;
      bool retry = !progress.transport_connected || failure == make_error_code(Error::target_session) || retry_timeout;
      if (!retry)
        co_await fail(failure);
    }
  }

  co_await fail(failure);
}

Task<Connection> Connection::establish_reported(
  detail::OwnedOptions pending,
  ConnectionReport &report,
  std::shared_ptr<NoticeReceiver> notices,
  std::shared_ptr<TraceReceiver> trace)
{
  report = {};
  Diagnostic diagnostic;
  auto connection = co_await as_result(
    establish(std::move(pending), &diagnostic, std::move(notices), std::move(trace), {}, &report));
  finish_report(report, connection ? std::error_code{} : connection.error(), diagnostic);
  if (!connection)
    co_await fail(connection.error());
  co_return std::move(*connection);
}

Task<void> Connection::reset_impl(
  detail::OwnedOptions pending,
  Diagnostic *diagnostic,
  Borrow borrow,
  ConnectionReport *report)
{
  auto &options = pending.value;
  PasswordCleanup cleanup{options};
  if (diagnostic)
    *diagnostic = {};
  if (auto valid = valid_options(options); !valid)
    co_await fail(valid.error());

  co_await cancellation_point();
  auto &original = state();
  if (original.busy.exchange(true, std::memory_order_acq_rel))
    co_await fail(Error::busy);

  struct ResetGuard {
    Impl *connection;

    ~ResetGuard()
    {
      release();
    }

    void release() noexcept
    {
      if (connection) {
        connection->busy.store(false, std::memory_order_release);
        connection = nullptr;
      }
    }
  } guard{&original};

  if (original.borrowers.load(std::memory_order_acquire) != 1)
    co_await fail(Error::busy);
  if (auto *secured = std::get_if<detail::GssStream>(&original.transport))
    co_await secured->finish();
  if (auto closed = original.close_transport(); !closed)
    co_await fail(closed.error());
  Diagnostic startup_error;
  auto replacement = co_await as_result(
    Connection::establish(
      std::move(pending),
      &startup_error,
      original.notice_receiver,
      original.trace_receiver,
      original.notification_receiver,
      report));
  if (!replacement) {
    original.error = std::move(startup_error);
    original.operation_error = replacement.error();
    if (diagnostic)
      *diagnostic = original.error;
    co_await fail(replacement.error());
  }
  guard.release();
  borrow.release();
  replacement->impl_->events = std::move(original.events);
  impl_ = std::move(replacement->impl_);
  if (impl_->events) {
    impl_->events->owner = this;
    impl_->events->reset();
  }
}

Task<void> Connection::reset(Options options)
{
  return reset_impl(detail::OwnedOptions{std::move(options)}, nullptr, Borrow{*this});
}

Task<void> Connection::reset(Options options, Diagnostic &diagnostic)
{
  return reset_impl(detail::OwnedOptions{std::move(options)}, &diagnostic, Borrow{*this});
}

Task<void> Connection::reset_reported(detail::OwnedOptions pending, ConnectionReport &report, Borrow borrow)
{
  report = {};
  Diagnostic diagnostic;
  auto reset = co_await as_result(reset_impl(std::move(pending), &diagnostic, std::move(borrow), &report));
  finish_report(report, reset ? std::error_code{} : reset.error(), diagnostic);
  if (!reset)
    co_await fail(reset.error());
}

Task<void> Connection::reset(Options options, ConnectionReport &report)
{
  return reset_reported(detail::OwnedOptions{std::move(options)}, report, Borrow{*this});
}

Task<ServerStatus> ping(Options options)
{
  return Connection::probe(detail::OwnedOptions{std::move(options)});
}

Task<Connection> connect(Options options, NoticeHandler notices)
{
  detail::OptionsCleanup cleanup{options};
  return connect(std::move(options), std::move(notices), Trace{});
}

Task<Connection> connect(Options options, Diagnostic &diagnostic, NoticeHandler notices)
{
  detail::OptionsCleanup cleanup{options};
  return connect(std::move(options), diagnostic, std::move(notices), Trace{});
}

Task<Connection> connect(Options options, NoticeHandler notices, Trace trace)
{
  auto receiver = notices ? std::make_shared<Connection::NoticeReceiver>(std::move(notices)) : nullptr;
  auto tracing = trace.handler || !valid_trace(trace) ? std::make_shared<Connection::TraceReceiver>(std::move(trace))
                                                      : nullptr;
  return Connection::establish(
    detail::OwnedOptions{std::move(options)},
    nullptr,
    std::move(receiver),
    std::move(tracing));
}

Task<Connection> connect(Options options, Diagnostic &diagnostic, NoticeHandler notices, Trace trace)
{
  auto receiver = notices ? std::make_shared<Connection::NoticeReceiver>(std::move(notices)) : nullptr;
  auto tracing = trace.handler || !valid_trace(trace) ? std::make_shared<Connection::TraceReceiver>(std::move(trace))
                                                      : nullptr;
  return Connection::establish(
    detail::OwnedOptions{std::move(options)},
    &diagnostic,
    std::move(receiver),
    std::move(tracing));
}

Task<Connection> connect(Options options, ConnectionReport &report, NoticeHandler notices)
{
  detail::OptionsCleanup cleanup{options};
  return connect(std::move(options), report, std::move(notices), Trace{});
}

Task<Connection> connect(Options options, ConnectionReport &report, NoticeHandler notices, Trace trace)
{
  auto receiver = notices ? std::make_shared<Connection::NoticeReceiver>(std::move(notices)) : nullptr;
  auto tracing = trace.handler || !valid_trace(trace) ? std::make_shared<Connection::TraceReceiver>(std::move(trace))
                                                      : nullptr;
  return Connection::establish_reported(
    detail::OwnedOptions{std::move(options)},
    report,
    std::move(receiver),
    std::move(tracing));
}

Task<Results> Connection::query(std::string sql)
{
  return state().query(std::move(sql), Borrow{*this});
}

Task<std::vector<Outcome>> Connection::query_outcomes(std::string sql)
{
  return state().query<true>(std::move(sql), Borrow{*this});
}

Task<ResultSet> Connection::execute(std::string sql, std::vector<Parameter> parameters, Format format)
{
  return state().execute(std::move(sql), std::move(parameters), format, false, Borrow{*this});
}

Task<Outcome> Connection::execute_outcome(std::string sql, std::vector<Parameter> parameters, Format format)
{
  return state().execute<true>(std::move(sql), std::move(parameters), format, false, Borrow{*this});
}

Task<void> Connection::prepare(std::string name, std::string sql, std::vector<u32> types)
{
  return state().prepare(std::move(name), std::move(sql), std::move(types), Borrow{*this});
}

Task<ResultSet> Connection::execute_prepared(std::string name, std::vector<Parameter> parameters, Format format)
{
  return state().execute(std::move(name), std::move(parameters), format, true, Borrow{*this});
}

Task<Outcome> Connection::execute_prepared_outcome(std::string name, std::vector<Parameter> parameters, Format format)
{
  return state().execute<true>(std::move(name), std::move(parameters), format, true, Borrow{*this});
}

Task<ResultSet> Connection::describe(std::string name)
{
  return state().describe(std::move(name), false, Borrow{*this});
}

Task<ResultSet> Connection::describe_portal(std::string name)
{
  return state().describe(std::move(name), true, Borrow{*this});
}

Task<void> Connection::close_prepared(std::string name)
{
  return state().close_prepared(std::move(name), Borrow{*this});
}

Task<Value> Connection::call_function(u32 function, std::vector<Parameter> parameters, Format format)
{
  return state().call_function(function, std::move(parameters), format, Borrow{*this});
}

Task<std::vector<Outcome>> Connection::batch(std::vector<Command> commands)
{
  return state().batch(std::move(commands), Borrow{*this});
}

Task<void> Connection::request_cancel()
{
  return cancel_request(cancel_handle());
}

Result<CancelHandle> Connection::cancel_handle() const
{
  if (!impl_ || !impl_->connected || !impl_->process || impl_->cancel_key.size() < 4)
    return std::unexpected(make_error_code(Error::closed));

  auto state = std::make_shared<CancelHandle::Impl>();
  state->endpoint = impl_->endpoint;
  state->local_address = impl_->local_address;
  state->peer_user = impl_->peer_user;
  state->host = impl_->options.host;
  state->tls = impl_->options.tls;
  state->tls_negotiation = impl_->options.tls_negotiation;
  state->server_name_indication = impl_->options.server_name_indication;
  state->client_certificate = impl_->options.client_certificate;
  state->gss_encrypted = std::holds_alternative<detail::GssStream>(impl_->transport);
  if (state->gss_encrypted) {
    state->gss = impl_->options.gss;
    state->gss_service = impl_->options.gss_service;
    state->gss_delegation = impl_->options.gss_delegation;
  }
  state->plaintext = std::holds_alternative<TcpStream>(impl_->transport) ||
    std::holds_alternative<LocalStream>(impl_->transport);
  state->deadline = impl_->options.connect_timeout;
  state->process = impl_->process;
  state->key = impl_->cancel_key;
  return CancelHandle{std::move(state)};
}

Task<CopyFormat> Connection::start_copy(std::string sql)
{
  return state().start_copy(std::move(sql), Borrow{*this});
}

Task<void> Connection::write_copy(std::span<const std::byte> data)
{
  return state().write_copy(data, state().copy_generation, Borrow{*this});
}

Task<void> Connection::finish_copy_send()
{
  return state().finish_copy_send(state().copy_generation, Borrow{*this});
}

Task<std::optional<std::vector<std::byte>>> Connection::read_copy()
{
  return state().read_copy(state().copy_generation, Borrow{*this});
}

Task<ResultSet> Connection::end_copy(std::optional<std::string> error)
{
  return state().end_copy(std::move(error), state().copy_generation, Borrow{*this});
}

Result<ResultSet> Connection::copy_result() const
{
  if (!state().copied || state().copied->empty())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  return state().copied->back();
}

Result<Results> Connection::copy_results() const
{
  if (!state().copied)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  return *state().copied;
}

Task<void> Connection::start_rows(std::string sql)
{
  return state().start_rows(std::move(sql), Borrow{*this});
}

Task<std::optional<Row>> Connection::read_row()
{
  return state().read_row(Borrow{*this});
}

const std::vector<Column> &Connection::row_columns() const noexcept
{
  return state().streaming_columns;
}

Task<ResultSet> Connection::open_portal(std::string name, std::string sql, std::vector<Parameter> parameters)
{
  return state().open_portal(std::move(name), std::move(sql), std::move(parameters), Borrow{*this});
}

Task<ResultSet> Connection::fetch(std::string name, u32 rows)
{
  return state().fetch(std::move(name), rows, Borrow{*this});
}

Task<void> Connection::close_portal(std::string name)
{
  return state().close_portal(std::move(name), Borrow{*this});
}

Task<Notification> Connection::wait_notification()
{
  return state().wait_notification(Borrow{*this});
}

Task<void> Connection::finish()
{
  return state().finish(Borrow{*this});
}

Result<void> Connection::close() noexcept
{
  if (!impl_)
    return {};
  if (impl_->busy.load(std::memory_order_acquire) || (impl_->events && impl_->events->invoking()))
    return std::unexpected(make_error_code(Error::busy));

  return impl_->close_transport();
}

Result<void> Connection::cancel() noexcept
{
  if (!impl_)
    return {};
  if (impl_->notice_receiver && impl_->notice_receiver->invoking)
    return std::unexpected(make_error_code(Error::busy));
  if (impl_->notification_receiver && impl_->notification_receiver->invoking)
    return std::unexpected(make_error_code(Error::busy));
  if (impl_->events && impl_->events->invoking())
    return std::unexpected(make_error_code(Error::busy));

  if (impl_->pipeline)
    impl_->pipeline->stop(std::make_error_code(std::errc::operation_canceled));
  impl_->connected = false;
  return std::visit(
    [](auto &stream) {
      return stream.cancel();
    },
    impl_->transport);
}

bool Connection::open() const noexcept
{
  return impl_ && impl_->connected;
}

Transaction Connection::transaction() const noexcept
{
  if (!open())
    return Transaction::unknown;

  auto &connection = *impl_;
  if (connection.pipeline) {
    auto &pipeline = *connection.pipeline;
    std::lock_guard lock{pipeline.mutex};
    if (!pipeline.pending.empty() || !pipeline.submitted.empty() || !pipeline.outbound.bytes.empty() ||
      pipeline.unsynchronized)
      return Transaction::in_progress;
  }
  return connection.awaiting_ready ? Transaction::in_progress : connection.transaction;
}

PipelineStatus Connection::pipeline_status() const noexcept
{
  if (!impl_ || !impl_->pipeline)
    return PipelineStatus::off;

  auto &pipeline = *impl_->pipeline;
  std::lock_guard lock{pipeline.mutex};
  return pipeline.aborted ? PipelineStatus::aborted : PipelineStatus::on;
}

u32 Connection::backend_process() const noexcept
{
  return state().process;
}

ProtocolVersion Connection::protocol_version() const noexcept
{
  return static_cast<ProtocolVersion>(196608 + state().protocol_minor);
}

Authentication Connection::authentication_method() const noexcept
{
  return state().authentication;
}

bool Connection::gss_encrypted() const noexcept
{
  return impl_ && std::holds_alternative<detail::GssStream>(impl_->transport);
}

Result<ConnectionInfo> Connection::info() const
{
  if (!impl_)
    return std::unexpected(make_error_code(Error::closed));
  auto &connection = *impl_;
  if (connection.borrowers.load(std::memory_order_acquire) != 0 ||
    connection.copy_writing.load(std::memory_order_acquire))
    return std::unexpected(make_error_code(Error::busy));

  Impl::Guard guard{connection};
  auto acquired = guard.acquire();
  guard.completed = true;
  if (!acquired)
    return std::unexpected(acquired.error());

  ConnectionInfo info;
  info.host = connection.options.host;
  info.port = connection.options.port;
  if (connection.local_address.empty())
    info.endpoint = connection.endpoint;
  else
    info.local_address = connection.local_address;
  info.peer_user = connection.peer_user;
  info.database = connection.options.database.empty() ? connection.options.user : connection.options.database;
  info.user = connection.options.user;
  info.server_options = connection.options.server_options;
  if (auto version = connection.server_parameters.find("server_version");
    version != connection.server_parameters.end()) {
    info.server_version = version->second;
    info.server_version_number = server_version_number(version->second);
  }
  info.backend_process = connection.process;
  info.protocol_version = static_cast<ProtocolVersion>(196608 + connection.protocol_minor);
  info.authentication = connection.authentication_info;
  info.transaction = transaction();
  info.gss_encrypted = std::holds_alternative<detail::GssStream>(connection.transport);
  if (auto *stream = std::get_if<TlsStream<TcpStream>>(&connection.transport)) {
    auto secured = stream->info();
    if (!secured)
      return std::unexpected(secured.error());
    info.tls = std::move(*secured);
  }

  return info;
}

Result<OptionsInfo> Connection::configuration() const
{
  if (!impl_)
    return std::unexpected(make_error_code(Error::closed));
  auto &connection = *impl_;
  if (connection.borrowers.load(std::memory_order_acquire) != 0 ||
    connection.copy_writing.load(std::memory_order_acquire) ||
    (connection.notice_receiver && connection.notice_receiver->invoking) ||
    (connection.notification_receiver && connection.notification_receiver->invoking) ||
    (connection.trace_receiver && connection.trace_receiver->invoking.load(std::memory_order_acquire)) ||
    (connection.events && connection.events->invoking()))
    return std::unexpected(make_error_code(Error::busy));
  if (connection.busy.exchange(true, std::memory_order_acq_rel))
    return std::unexpected(make_error_code(Error::busy));

  struct Release {
    std::atomic<bool> &busy;

    ~Release()
    {
      busy.store(false, std::memory_order_release);
    }
  } release{connection.busy};

  return connection.configuration;
}

Result<Encoding> Connection::client_encoding() const noexcept
{
  if (!open())
    return std::unexpected(make_error_code(Error::closed));
  auto parameter = impl_->server_parameters.find("client_encoding");
  if (parameter == impl_->server_parameters.end())
    return std::unexpected(make_error_code(Error::protocol));
  auto encoding = parse_encoding(parameter->second);
  if (!encoding)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  return *encoding;
}

Task<void> Connection::set_client_encoding(Encoding encoding)
{
  return state().set_encoding(encoding, Borrow{*this});
}

Task<std::string> Connection::password_verifier(std::string user, std::string_view password, PasswordOptions options)
{
  return state().password<false>(std::move(user), detail::own_password(password), options, Borrow{*this});
}

Task<ResultSet> Connection::change_password(std::string user, std::string_view password, PasswordOptions options)
{
  return state().password<true>(std::move(user), detail::own_password(password), options, Borrow{*this});
}

Result<std::optional<std::string>> Connection::parameter(std::string_view name) const
{
  if (!impl_)
    return std::unexpected(make_error_code(Error::closed));
  auto &connection = *impl_;
  if (connection.borrowers.load(std::memory_order_acquire) != 0 ||
    connection.copy_writing.load(std::memory_order_acquire))
    return std::unexpected(make_error_code(Error::busy));

  Impl::Guard guard{connection};
  auto acquired = guard.acquire();
  guard.completed = true;
  if (!acquired)
    return std::unexpected(acquired.error());
  if (name.find('\0') != std::string_view::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto value = connection.server_parameters.find(name);
  if (value == connection.server_parameters.end())
    return std::optional<std::string>{};
  return std::optional<std::string>{value->second};
}

Diagnostic Connection::last_error() const
{
  return state().error;
}

Result<Failure> Connection::last_failure() const
{
  if (!impl_)
    return std::unexpected(make_error_code(Error::closed));
  auto &connection = *impl_;
  if (connection.borrowers.load(std::memory_order_acquire) != 0 ||
    connection.copy_writing.load(std::memory_order_acquire) ||
    (connection.notice_receiver && connection.notice_receiver->invoking) ||
    (connection.notification_receiver && connection.notification_receiver->invoking) ||
    (connection.trace_receiver && connection.trace_receiver->invoking.load(std::memory_order_acquire)) ||
    (connection.events && connection.events->invoking()))
    return std::unexpected(make_error_code(Error::busy));
  if (connection.busy.exchange(true, std::memory_order_acq_rel))
    return std::unexpected(make_error_code(Error::busy));

  struct Release {
    std::atomic<bool> &busy;

    ~Release()
    {
      busy.store(false, std::memory_order_release);
    }
  } release{connection.busy};

  auto failure = connection.operation_error;
  if (!failure && !connection.error.fields.empty())
    failure = sql_error(connection.error.sqlstate());
  return Failure{failure, connection.error};
}

Result<NoticeHandler> Connection::on_notice(NoticeHandler handler)
{
  auto &connection = state();
  if (connection.busy.load(std::memory_order_acquire) || connection.copy_writing.load(std::memory_order_acquire) ||
    connection.borrowers.load(std::memory_order_acquire) != 0 ||
    (connection.notice_receiver && connection.notice_receiver->invoking) ||
    (connection.events && connection.events->invoking()))
    return std::unexpected(make_error_code(Error::busy));

  auto receiver = handler ? std::make_shared<NoticeReceiver>(std::move(handler)) : nullptr;
  auto previous = std::exchange(connection.notice_receiver, std::move(receiver));
  if (previous)
    return std::move(previous->handler);
  return NoticeHandler{};
}

Result<Trace> Connection::on_trace(Trace trace)
{
  auto &connection = state();
  if (connection.busy.load(std::memory_order_acquire) || connection.copy_writing.load(std::memory_order_acquire) ||
    connection.borrowers.load(std::memory_order_acquire) != 0 ||
    (connection.trace_receiver && connection.trace_receiver->invoking.load(std::memory_order_acquire)) ||
    (connection.events && connection.events->invoking()))
    return std::unexpected(make_error_code(Error::busy));
  if (!valid_trace(trace))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto receiver = trace.handler ? std::make_shared<TraceReceiver>(std::move(trace)) : nullptr;
  auto previous = std::exchange(connection.trace_receiver, std::move(receiver));
  if (previous)
    return std::move(previous->configuration);
  return Trace{};
}

Result<NotificationHandler> Connection::on_notification(NotificationHandler handler)
{
  auto &connection = state();
  if (connection.busy.load(std::memory_order_acquire) || connection.copy_writing.load(std::memory_order_acquire) ||
    connection.borrowers.load(std::memory_order_acquire) != 0 ||
    (connection.notification_receiver && connection.notification_receiver->invoking) ||
    (connection.events && connection.events->invoking()))
    return std::unexpected(make_error_code(Error::busy));

  auto receiver = handler ? std::make_shared<NotificationReceiver>(std::move(handler)) : nullptr;
  auto previous = std::exchange(connection.notification_receiver, std::move(receiver));
  if (previous)
    return std::move(previous->handler);
  return NotificationHandler{};
}

Result<EventId> Connection::on_event(std::string name, EventHandler handler)
{
  auto &connection = state();
  if (connection.busy.load(std::memory_order_acquire) || connection.copy_writing.load(std::memory_order_acquire) ||
    connection.borrowers.load(std::memory_order_acquire) != 0 || (connection.events && connection.events->invoking()))
    return std::unexpected(make_error_code(Error::busy));

  if (!connection.events)
    connection.events = std::make_unique<detail::ConnectionEvents>(*this);
  auto registered = connection.events->add(std::move(name), std::move(handler));
  if (!registered && connection.events->entries.empty())
    connection.events.reset();
  return registered;
}

Result<void> Connection::attach_events(ResultSet &result)
{
  auto &connection = state();
  if (connection.busy.load(std::memory_order_acquire) || connection.copy_writing.load(std::memory_order_acquire) ||
    connection.borrowers.load(std::memory_order_acquire) != 0 || (connection.events && connection.events->invoking()))
    return std::unexpected(make_error_code(Error::busy));

  const std::vector<detail::EventEntry> empty;
  const auto &entries = connection.events ? connection.events->entries : empty;
  return detail::EventAccess::attach(result, *this, entries);
}

Result<EventData> Connection::event_data(EventId id) const
{
  const auto &connection = state();
  if (!connection.events)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return connection.events->data(id);
}

Result<void> Connection::set_event_data(EventId id, EventData data)
{
  auto &connection = state();
  if (connection.busy.load(std::memory_order_acquire) || connection.copy_writing.load(std::memory_order_acquire) ||
    connection.borrowers.load(std::memory_order_acquire) != 0)
    return std::unexpected(make_error_code(Error::busy));
  if (!connection.events)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return connection.events->set_data(id, std::move(data));
}

Result<std::string> Connection::escape_literal(std::string_view text) const
{
  auto encoding = client_encoding();
  if (!encoding)
    return std::unexpected(encoding.error());
  return pg::escape_literal(text, *encoding, impl_->options.limits.message_bytes);
}

Result<std::string> Connection::escape_identifier(std::string_view text) const
{
  auto encoding = client_encoding();
  if (!encoding)
    return std::unexpected(encoding.error());
  return pg::escape_identifier(text, *encoding, impl_->options.limits.message_bytes);
}

std::vector<Diagnostic> Connection::take_notices()
{
  state().notice_bytes = 0;
  return std::exchange(state().notices, {});
}

std::vector<Notification> Connection::take_notifications()
{
  state().notification_bytes = 0;
  return std::exchange(state().notifications, {});
}

} // namespace weave::pg
