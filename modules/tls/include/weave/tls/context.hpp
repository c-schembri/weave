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

enum class TlsVersion {
  tls12,
  tls13
};

enum class TlsVerification {
  none,
  certificate,
  hostname
};

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

Error make_error_code(TlsError error) noexcept;

enum class TlsClientAuth {
  none,
  optional,
  required
};

// Client identity use, separate from the server's client-authentication requirement.
enum class TlsCertificateMode {
  disable,
  allow,
  require
};

enum class TlsRevocation {
  none,
  leaf,
  chain
};

enum class TlsPrivateKeyFormat {
  pem,
  der,
  // A URI/path handled by an already configured OpenSSL STORE provider.
  store
};

enum class TlsOcsp {
  none,
  if_present,
  required
};

enum class TlsSessionMode {
  disabled,
  stateful,
  tickets
};

struct TlsLimits {
  std::size_t buffered_input = 256 * 1024;
  std::size_t buffered_output = 256 * 1024;
  std::size_t certificate_chain = 64 * 1024;
  int verification_depth = 16;
};

struct TlsCipherPolicy {
  std::string tls12;
  std::string tls13;
  std::string groups;
  std::string signatures;
  int security_level = 2;
};

struct TlsSessionOptions {
  TlsSessionMode mode = TlsSessionMode::disabled;
  std::size_t capacity = 1024;
  std::chrono::seconds lifetime{600};
  std::size_t tickets = 2;
};

struct TlsSessionStats {
  u64 handshakes = 0;
  u64 resumed = 0;
};

struct TlsPeerIdentity {
  std::string subject;
  std::string issuer;
  std::vector<std::byte> sha256;
  std::vector<std::byte> certificate;
};

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

class TlsPasswordProvider {
  friend struct detail::TlsPasswordAccess;
  struct Impl;
  std::shared_ptr<const Impl> impl_;

  explicit TlsPasswordProvider(std::shared_ptr<const Impl> impl) noexcept;

public:
  // Invoked synchronously by the key loader. The path/URI is borrowed through invocation.
  // Copies share the const callable; callers must synchronize mutable captured state.
  using Handler = std::move_only_function<Result<std::string>(std::string_view private_key_file) const noexcept>;

  [[nodiscard]] static Result<TlsPasswordProvider> create(Handler handler);
};

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

  bool available() const noexcept;
  std::chrono::seconds remaining() const noexcept;
};

class TlsContext {
  friend class detail::TlsEngine;
  std::shared_ptr<detail::TlsCredentials> credentials_;

  explicit TlsContext(std::shared_ptr<detail::TlsCredentials> credentials) noexcept
      : credentials_(std::move(credentials))
  {
  }

public:
  static Result<TlsContext> client(TlsClientOptions options = {});
  static Result<TlsContext> server(TlsServerOptions options);

  TlsSessionStats session_stats() const noexcept;
  TlsVerification verification() const noexcept;
  Result<void> clear_sessions() const noexcept;
};

} // namespace weave

namespace std {

template <>
struct is_error_code_enum<weave::TlsError> : true_type {};

} // namespace std
