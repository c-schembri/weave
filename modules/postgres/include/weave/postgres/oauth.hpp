#pragma once

#include <weave/task.hpp>
#include <weave/types.hpp>
#include <weave/tls/context.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <chrono>

namespace weave::pg {

namespace detail {

struct OAuthDeviceAccess;

} // namespace detail

/// Shared immutable client credential; the final owner best-effort cleanses its storage.
class OAuthClientSecret {
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  explicit OAuthClientSecret(std::shared_ptr<const Impl> impl) noexcept;

public:
  /// Validate and copy borrowed secret bytes synchronously; source copies are not cleansed by this owner.
  [[nodiscard]] static Result<OAuthClientSecret> parse(std::string_view secret);
  // Copies share immutable credential bytes; the final owner cleanses storage.
  OAuthClientSecret(OAuthClientSecret &&) noexcept;
  OAuthClientSecret &operator=(OAuthClientSecret &&) noexcept;
  OAuthClientSecret(const OAuthClientSecret &) noexcept = default;
  OAuthClientSecret &operator=(const OAuthClientSecret &) noexcept = default;
  ~OAuthClientSecret();

  /// Borrow sensitive bytes until this credential's backing storage is released; never log them.
  std::string_view value() const noexcept;
};

/// Move-only owned device-flow prompt. Accessor views expire on movement/destruction.
class OAuthDevicePrompt {
  friend struct detail::OAuthDeviceAccess;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit OAuthDevicePrompt(std::unique_ptr<Impl> impl) noexcept;

public:
  OAuthDevicePrompt(OAuthDevicePrompt &&) noexcept;
  OAuthDevicePrompt &operator=(OAuthDevicePrompt &&) noexcept;
  OAuthDevicePrompt(const OAuthDevicePrompt &) = delete;
  ~OAuthDevicePrompt();

  /// Borrow the authorization URI for user presentation.
  std::string_view verification_uri() const noexcept;
  /// Borrow the device-flow user code; not an access token.
  std::string_view user_code() const noexcept;
  /// Borrow the complete authorization URI when provided, otherwise an empty view.
  std::string_view verification_uri_complete() const noexcept;
  /// Local steady-clock prompt expiry; does not extend server authorization lifetime.
  std::chrono::steady_clock::time_point expires_at() const noexcept;
};

/// OAuth token-endpoint client authentication policy; automatic selects from configured credentials.
enum class OAuthClientAuth {
  automatic,
  none,
  client_secret_basic,
  client_secret_post
};

/// Owned verified-HTTPS device-flow transport/authentication policy and per-request timeout.
struct OAuthDeviceOptions {
  std::optional<TlsContext> tls;
  std::optional<OAuthClientSecret> client_secret;
  OAuthClientAuth client_auth = OAuthClientAuth::automatic;
  std::chrono::milliseconds request_timeout{30000};
};

/// Move-only owned access token; destruction best-effort cleanses this storage, not caller copies.
class OAuthToken {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit OAuthToken(std::unique_ptr<Impl> impl) noexcept;

public:
  /// Validate and copy borrowed credential bytes into owned storage synchronously.
  [[nodiscard]] static Result<OAuthToken> parse(std::string_view token);
  OAuthToken(OAuthToken &&) noexcept;
  OAuthToken &operator=(OAuthToken &&) noexcept;
  OAuthToken(const OAuthToken &) = delete;
  ~OAuthToken();

  /// Borrow credential bytes until movement/destruction; unmanaged copies require caller protection.
  std::string_view value() const noexcept;
};

/// Owning token acquisition metadata, including the configured issuer trust decision and session identity.
struct OAuthRequest {
  std::string issuer;
  std::string client_id;
  std::string scope;
  std::string openid_configuration;
  std::string host;
  u16 port = 0;
  std::string user;
  std::string database;
  std::optional<OAuthClientSecret> client_secret;
  bool scope_explicit = false;
};

/// Shared owned const noexcept callbacks; captures remain caller-owned if they contain references.
/// Concurrent acquisitions share callables, not a callback lock; synchronize captured mutable state.
class OAuthProvider {
  struct Impl;
  std::shared_ptr<const Impl> impl_;

  explicit OAuthProvider(std::shared_ptr<const Impl> impl) noexcept;
  static Task<OAuthToken> acquire(std::shared_ptr<const Impl> impl, OAuthRequest request);

public:
  /// Acquire asynchronously from an owned request; return an owned token or task error.
  using Factory = std::move_only_function<Task<OAuthToken>(OAuthRequest) const noexcept>;
  /// Present/approve an owned device prompt asynchronously; cancellation must be cooperative.
  using DeviceHandler = std::move_only_function<Task<void>(OAuthDevicePrompt) const noexcept>;
  /// Memory-only synchronous cache lookup; request is borrowed for the invocation only.
  using CacheLookup = std::move_only_function<Result<std::optional<OAuthToken>>(const OAuthRequest &) const noexcept>;

  /// Own factory/cache callbacks in shared state; reject invalid callbacks without starting acquisition.
  [[nodiscard]] static Result<OAuthProvider> create(Factory factory, CacheLookup cached = {});
  /// Own prompt callback/options/cache and construct a bounded verified-HTTPS device-flow provider.
  /// Caller still owns issuer trust, cache policy and any referenced callback state.
  [[nodiscard]] static Result<OAuthProvider> device(
    DeviceHandler handler,
    OAuthDeviceOptions options = {},
    CacheLookup cached = {});
  /// Synchronous memory-only lookup: nullopt is a miss; callback errors are terminal, not misses.
  /// Borrows request only during this call; returned tokens own their bytes.
  [[nodiscard]] Result<std::optional<OAuthToken>> cached_token(const OAuthRequest &request) const;
  /// Copy/move request and retain provider state before initial suspension; source handle need not survive.
  /// Captured references still must outlive acquisition; returned token owns credential storage.
  [[nodiscard]] Task<OAuthToken> request(OAuthRequest request) const;
};

/// Owned PostgreSQL OAuth policy. Explicit issuer/client identity and application provider control trust.
struct OAuthOptions {
  std::string issuer;
  std::string client_id;
  std::optional<std::string> scope;
  std::optional<OAuthClientSecret> client_secret;
  std::optional<OAuthProvider> provider;
  std::chrono::milliseconds acquisition_timeout{std::chrono::minutes{30}};
};

} // namespace weave::pg
