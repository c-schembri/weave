#pragma once

#include <weave/tls/context.hpp>
#include <openssl/ssl.h>
#include <atomic>

namespace weave::detail {

class TlsKeyLog;

struct TlsCredentials {
  SSL_CTX *context = nullptr;
  bool server = false;
  bool resumption = false;
  TlsClientAuth client_auth = TlsClientAuth::none;
  TlsOcsp ocsp = TlsOcsp::none;
  TlsSessionMode session_mode = TlsSessionMode::disabled;
  TlsLimits limits;
  std::chrono::seconds session_lifetime{600};
  std::chrono::seconds ocsp_max_age{3600};
  std::chrono::seconds ocsp_clock_skew{60};
  std::vector<unsigned char> alpn;
  std::vector<unsigned char> ocsp_response;
  std::atomic<u64> handshakes{0};
  std::atomic<u64> resumed{0};
  TlsVerification verification = TlsVerification::hostname;
  std::shared_ptr<TlsKeyLog> key_log;

  ~TlsCredentials();
};

struct TlsSessionState {
  SSL_SESSION *session = nullptr;
  std::shared_ptr<TlsCredentials> credentials;
  std::string name;
  std::string required_protocol;
  bool server_name_indication = true;
  TlsCertificateMode client_certificate = TlsCertificateMode::allow;
  bool client_certificate_used = false;
  std::vector<X509 *> chain;
  std::atomic<bool> offered{false};
  std::atomic<bool> revoked{false};

  ~TlsSessionState();
};

Error tls_native_error() noexcept;
Result<void> tls_verify_chain(SSL *ssl, const TlsCredentials &credentials, std::span<X509 *const> saved = {});
Result<void> tls_verify_ocsp(SSL *ssl, const TlsCredentials &credentials);
Result<void> tls_configure_sessions(TlsCredentials &credentials, const TlsSessionOptions &options);
bool tls_staple_ready(const TlsCredentials &credentials) noexcept;

} // namespace weave::detail
