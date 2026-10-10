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

class OAuthClientSecret {
  struct Impl;
  std::shared_ptr<const Impl> impl_;
  explicit OAuthClientSecret(std::shared_ptr<const Impl> impl) noexcept;

public:
  [[nodiscard]] static Result<OAuthClientSecret> parse(std::string_view secret);
  // Copies share immutable credential bytes; the final owner cleanses storage.
  OAuthClientSecret(OAuthClientSecret &&) noexcept;
  OAuthClientSecret &operator=(OAuthClientSecret &&) noexcept;
  OAuthClientSecret(const OAuthClientSecret &) noexcept = default;
  OAuthClientSecret &operator=(const OAuthClientSecret &) noexcept = default;
  ~OAuthClientSecret();

  // Borrowed bytes: caller-created string copies are not cleansed by this owner.
  std::string_view value() const noexcept;
};

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

  std::string_view verification_uri() const noexcept;
  std::string_view user_code() const noexcept;
  std::string_view verification_uri_complete() const noexcept;
  std::chrono::steady_clock::time_point expires_at() const noexcept;
};

enum class OAuthClientAuth {
  automatic,
  none,
  client_secret_basic,
  client_secret_post
};

struct OAuthDeviceOptions {
  std::optional<TlsContext> tls;
  std::optional<OAuthClientSecret> client_secret;
  OAuthClientAuth client_auth = OAuthClientAuth::automatic;
  std::chrono::milliseconds request_timeout{30000};
};

class OAuthToken {
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit OAuthToken(std::unique_ptr<Impl> impl) noexcept;

public:
  [[nodiscard]] static Result<OAuthToken> parse(std::string_view token);
  OAuthToken(OAuthToken &&) noexcept;
  OAuthToken &operator=(OAuthToken &&) noexcept;
  OAuthToken(const OAuthToken &) = delete;
  ~OAuthToken();

  // Borrowed credential bytes: caller-created copies are not cleansed by this owner.
  std::string_view value() const noexcept;
};

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

class OAuthProvider {
  struct Impl;
  std::shared_ptr<const Impl> impl_;

  explicit OAuthProvider(std::shared_ptr<const Impl> impl) noexcept;
  static Task<OAuthToken> acquire(std::shared_ptr<const Impl> impl, OAuthRequest request);

public:
  using Factory = std::move_only_function<Task<OAuthToken>(OAuthRequest) const noexcept>;
  using DeviceHandler = std::move_only_function<Task<void>(OAuthDevicePrompt) const noexcept>;
  using CacheLookup = std::move_only_function<Result<std::optional<OAuthToken>>(const OAuthRequest &) const noexcept>;

  [[nodiscard]] static Result<OAuthProvider> create(Factory factory, CacheLookup cached = {});
  [[nodiscard]] static Result<OAuthProvider> device(
    DeviceHandler handler,
    OAuthDeviceOptions options = {},
    CacheLookup cached = {});
  // Synchronous, memory-only lookup. An empty optional is a miss; errors are terminal.
  [[nodiscard]] Result<std::optional<OAuthToken>> cached_token(const OAuthRequest &request) const;
  // Retains the callable and request before the lazy Task's initial suspension.
  [[nodiscard]] Task<OAuthToken> request(OAuthRequest request) const;
};

struct OAuthOptions {
  std::string issuer;
  std::string client_id;
  std::optional<std::string> scope;
  std::optional<OAuthClientSecret> client_secret;
  std::optional<OAuthProvider> provider;
  std::chrono::milliseconds acquisition_timeout{std::chrono::minutes{30}};
};

} // namespace weave::pg
