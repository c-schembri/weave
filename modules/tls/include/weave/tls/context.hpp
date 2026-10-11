#pragma once

#include <weave/task.hpp>
#include <weave/types.hpp>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <span>
#include <optional>
#include <functional>
#include <string_view>

namespace weave {

/// Supported protocol versions; older TLS versions and early data are not offered.
enum class TlsVersion {
  tls12,
  tls13
};

/// Client trust policy: none disables authentication, certificate checks trust, hostname also checks identity.
enum class TlsVerification {
  none,
  certificate,
  hostname
};

/// TLS-specific failures, distinct from transport/native error categories.
enum class TlsError {
  protocol = 1,
  certificate_verification,
  truncated,
  closed,
  resource_limit,
  session_unavailable,
  session_rejected,
  revocation,
  client_certificate_required
};

/// Convert a TLS failure to Weave's standard error-code channel.
Error make_error_code(TlsError error) noexcept;

/// Server mTLS policy. optional/required verification needs explicitly configured client trust.
enum class TlsClientAuth {
  none,
  optional,
  required
};

/// Per-handshake client identity use, separate from the server's client-authentication requirement.
enum class TlsCertificateMode {
  disable,
  allow,
  require
};

/// CRL enforcement for the peer leaf or full chain; configured revocation data must be trusted.
enum class TlsRevocation {
  none,
  leaf,
  chain
};

/// Identity key input format; store uses an already configured OpenSSL provider, not an installed engine.
enum class TlsPrivateKeyFormat {
  pem,
  der,
  // A URI/path handled by an already configured OpenSSL STORE provider.
  store
};

/// Client stapled-OCSP policy. required rejects absent evidence; enforcement is incompatible with resumption.
enum class TlsOcsp {
  none,
  if_present,
  required
};

/// Opt-in server resumption mode; disabled is the default.
enum class TlsSessionMode {
  disabled,
  stateful,
  tickets
};

/// Logical TLS buffer/certificate limits, not a bound on total OpenSSL or process allocations.
struct TlsLimits {
  std::size_t buffered_input = 256 * 1024;
  std::size_t buffered_output = 256 * 1024;
  std::size_t certificate_chain = 64 * 1024;
  int verification_depth = 16;
};

/// Owned OpenSSL cipher/group/signature policy strings; empty strings retain library defaults.
struct TlsCipherPolicy {
  std::string tls12;
  std::string tls13;
  std::string groups;
  std::string signatures;
  int security_level = 2;
};

/// Server session cache/ticket limits. Clearing cache does not revoke stateless tickets.
struct TlsSessionOptions {
  TlsSessionMode mode = TlsSessionMode::disabled;
  std::size_t capacity = 1024;
  std::chrono::seconds lifetime{600};
  std::size_t tickets = 2;
};

/// Credential-context handshake and resumption counters, returned as a value snapshot.
struct TlsSessionStats {
  u64 handshakes = 0;
  u64 resumed = 0;
};

/// Owning authenticated peer identity, including certificate bytes and its SHA-256 fingerprint.
struct TlsPeerIdentity {
  std::string subject;
  std::string issuer;
  std::vector<std::byte> sha256;
  std::vector<std::byte> certificate;
};

/// Owning negotiated-session snapshot. Encryption alone does not imply certificate/hostname verification.
struct TlsInfo {
  std::string library;
  TlsVersion version = TlsVersion::tls13;
  std::string cipher;
  u32 key_bits = 0;
  bool compression = false;
  std::string negotiated_protocol;
  bool session_reused = false;
  std::optional<TlsPeerIdentity> peer;
  bool certificate_verified = false;
  bool hostname_verified = false;
};

/// Owned per-handshake policy. Timeout must be positive and at most 24 hours.
/// Disabling SNI changes routing disclosure only, never hostname verification.
struct TlsHandshakeOptions {
  std::chrono::milliseconds timeout{30000};
  // Clients offer only this ALPN value; both roles reject a different or absent selection.
  std::string required_protocol;
  // Client routing extension only; does not change the context's verification policy.
  bool server_name_indication = true;
  TlsCertificateMode client_certificate = TlsCertificateMode::allow;
};

namespace detail {

struct TlsPasswordAccess;

} // namespace detail

/// Shared immutable callback ownership for encrypted-key loading; captured references remain borrowed.
class TlsPasswordProvider {
  friend struct detail::TlsPasswordAccess;
  struct Impl;
  std::shared_ptr<const Impl> impl_;

  explicit TlsPasswordProvider(std::shared_ptr<const Impl> impl) noexcept;

public:
  /// Invoked synchronously by the key loader; path/URI is borrowed for this invocation only.
  /// Copies share the const callable; callers synchronize mutable captured state.
  using Handler = std::move_only_function<Result<std::string>(std::string_view private_key_file) const noexcept>;

  /// Consume a noexcept handler or return a validation/setup error. No stdin fallback on failure.
  [[nodiscard]] static Result<TlsPasswordProvider> create(Handler handler);
};

/// Owning client credential policy, loaded synchronously by TlsContext::client().
/// Defaults verify trust and hostname; resumption and secret key logging require explicit opt-in.
/// Passphrases are best-effort cleansed in owned storage, not in caller copies or allocator history.
struct TlsClientOptions {
  std::string ca_file;
  std::vector<std::string> alpn;
  TlsVersion min_version = TlsVersion::tls12;
  TlsVersion max_version = TlsVersion::tls13;
  std::string certificate_file;
  std::string private_key_file;
  std::string private_key_password;
  std::string ca_directory;
  std::string crl_file;
  // OpenSSL hashed verification directory; its contents must be trusted.
  std::string crl_directory;
  TlsRevocation revocation = TlsRevocation::none;
  TlsOcsp ocsp = TlsOcsp::none;
  bool session_resumption = false;
  std::chrono::seconds session_lifetime{600};
  std::chrono::seconds ocsp_max_age{3600};
  std::chrono::seconds ocsp_clock_skew{60};
  TlsLimits limits;
  TlsCipherPolicy ciphers;
  std::optional<TlsPasswordProvider> private_key_password_provider;
  TlsPrivateKeyFormat private_key_format = TlsPrivateKeyFormat::pem;
  TlsVerification verification = TlsVerification::hostname;
  // Explicit debugging only: this file contains traffic-decryption secrets.
  std::string key_log_file;
};

/// Owning server identity/trust/session policy, loaded synchronously by TlsContext::server().
/// mTLS requires explicit client trust. Key logging writes traffic-decryption secrets.
struct TlsServerOptions {
  std::string certificate_file;
  std::string private_key_file;
  std::string private_key_password;
  std::vector<std::string> alpn;
  TlsVersion min_version = TlsVersion::tls12;
  TlsVersion max_version = TlsVersion::tls13;
  TlsClientAuth client_auth = TlsClientAuth::none;
  std::string ca_file;
  std::string ca_directory;
  std::string crl_file;
  // OpenSSL hashed verification directory; its contents must be trusted.
  std::string crl_directory;
  TlsRevocation revocation = TlsRevocation::none;
  std::string ocsp_file;
  TlsSessionOptions sessions;
  TlsLimits limits;
  TlsCipherPolicy ciphers;
  std::optional<TlsPasswordProvider> private_key_password_provider;
  TlsPrivateKeyFormat private_key_format = TlsPrivateKeyFormat::pem;
  std::string key_log_file;
};

namespace detail {

struct TlsCredentials;
struct TlsSessionState;
class TlsEngine;

} // namespace detail

/// Move-only, one-shot resumption credential bound to credentials, server name and handshake policy.
/// Possession does not guarantee the peer accepts resumption; certificate checks still apply.
class TlsSession {
  friend class detail::TlsEngine;
  std::shared_ptr<detail::TlsSessionState> state_;

  explicit TlsSession(std::shared_ptr<detail::TlsSessionState> state) noexcept : state_(std::move(state))
  {
  }

public:
  TlsSession(TlsSession &&) noexcept = default;
  TlsSession(const TlsSession &) = delete;
  TlsSession &operator=(TlsSession &&) noexcept = default;

  /// Whether the local offer is unconsumed and unexpired at this instant.
  bool available() const noexcept;
  /// Remaining locally capped lifetime; zero for an unavailable offer.
  std::chrono::seconds remaining() const noexcept;
};

/// Cheaply copyable immutable credential snapshot shared by streams and concurrent handshakes.
/// This is TLS policy ownership, not an I/O execution Context.
class TlsContext {
  friend class detail::TlsEngine;
  std::shared_ptr<detail::TlsCredentials> credentials_;

  explicit TlsContext(std::shared_ptr<detail::TlsCredentials> credentials) noexcept
      : credentials_(std::move(credentials))
  {
  }

public:
  /// Synchronously validate/load an owning policy: copy lvalues, move rvalues.
  /// The returned snapshot retains credentials/providers, not references to options.
  static Result<TlsContext> client(TlsClientOptions options = {});
  /// Synchronously load an owning server identity/policy, returning validation or credential errors.
  static Result<TlsContext> server(TlsServerOptions options);

  /// Snapshot shared handshake/resumption counters without performing I/O.
  TlsSessionStats session_stats() const noexcept;
  /// Configured client verification policy, not proof a particular peer was verified.
  TlsVerification verification() const noexcept;
  /// Clear stateful session cache; does not revoke stateless tickets. Rotate credentials for revocation.
  Result<void> clear_sessions() const noexcept;
};

} // namespace weave

namespace std {

template <>
struct is_error_code_enum<weave::TlsError> : true_type {};

} // namespace std
