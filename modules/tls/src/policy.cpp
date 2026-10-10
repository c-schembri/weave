#include "credentials.hpp"
#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/x509v3.h>
#include <openssl/rand.h>
#include <array>
#include <ctime>
#include <chrono>
#include <limits>

namespace weave {

namespace {

template <class T, auto Release>
using Native = std::unique_ptr<T, decltype(Release)>;

Result<void> revoked() noexcept
{
  return std::unexpected(make_error_code(TlsError::revocation));
}

std::time_t session_time(const SSL_SESSION *session) noexcept
{
  return SSL_SESSION_get_time_ex(session);
}

std::time_t wall_time() noexcept
{
  // Linux time() can lag OpenSSL's high-resolution realtime at a second boundary.
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

bool current(const detail::TlsSessionState &state) noexcept
{
  if (!state.session || !SSL_SESSION_is_resumable(state.session) || state.offered.load(std::memory_order_acquire) ||
    state.revoked.load(std::memory_order_acquire))
    return false;
  const auto now = wall_time();
  const auto start = session_time(state.session);
  const auto lifetime = SSL_SESSION_get_timeout(state.session);
  if (now < start || lifetime <= 0 || now - start >= lifetime)
    return false;
  for (auto *certificate : state.chain) {
    if (X509_cmp_current_time(X509_get0_notBefore(certificate)) >= 0 ||
      X509_cmp_current_time(X509_get0_notAfter(certificate)) <= 0)
      return false;
  }
  return true;
}

Native<OCSP_RESPONSE, OCSP_RESPONSE_free> response(std::span<const unsigned char> encoded)
{
  const auto *cursor = encoded.data();
  auto value = Native<OCSP_RESPONSE, OCSP_RESPONSE_free>{
    d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(encoded.size())),
    OCSP_RESPONSE_free};
  if (!value || cursor != encoded.data() + encoded.size() ||
    OCSP_response_status(value.get()) != OCSP_RESPONSE_STATUS_SUCCESSFUL)
    return {nullptr, OCSP_RESPONSE_free};
  return value;
}

} // namespace

detail::TlsCredentials::~TlsCredentials()
{
  if (context)
    SSL_CTX_free(context);
}

detail::TlsSessionState::~TlsSessionState()
{
  SSL_SESSION_free(session);
  for (auto *certificate : chain)
    X509_free(certificate);
}

bool TlsSession::available() const noexcept
{
  // Captured sessions are immutable copies, never mutated by a live SSL or its cache.
  return state_ && current(*state_);
}

std::chrono::seconds TlsSession::remaining() const noexcept
{
  if (!available())
    return {};
  const auto age = wall_time() - session_time(state_->session);
  const auto lifetime = SSL_SESSION_get_timeout(state_->session);
  return std::chrono::seconds{age >= 0 && age < lifetime ? lifetime - age : 0};
}

TlsSessionStats TlsContext::session_stats() const noexcept
{
  if (!credentials_)
    return {};
  return {
    credentials_->handshakes.load(std::memory_order_relaxed),
    credentials_->resumed.load(std::memory_order_relaxed)};
}

TlsVerification TlsContext::verification() const noexcept
{
  return credentials_ ? credentials_->verification : TlsVerification::none;
}

Result<void> TlsContext::clear_sessions() const noexcept
{
  if (!credentials_ || !credentials_->server)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (credentials_->session_mode == TlsSessionMode::tickets)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  SSL_CTX_flush_sessions_ex(credentials_->context, (std::numeric_limits<std::time_t>::max)());
  return {};
}

Result<void> detail::tls_configure_sessions(TlsCredentials &credentials, const TlsSessionOptions &options)
{
  if ((options.mode != TlsSessionMode::disabled && options.mode != TlsSessionMode::stateful &&
        options.mode != TlsSessionMode::tickets) ||
    options.capacity == 0 || options.capacity > 65536 || options.lifetime.count() < 1 ||
    options.lifetime > std::chrono::hours{24} || options.tickets > 16 ||
    (options.mode != TlsSessionMode::disabled && options.tickets == 0))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  credentials.session_mode = options.mode;
  auto *context = credentials.context;
  SSL_CTX_set_timeout(context, static_cast<long>(options.lifetime.count()));
  SSL_CTX_sess_set_cache_size(context, static_cast<long>(options.capacity));
  if (options.mode == TlsSessionMode::disabled) {
    SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
  } else if (options.mode == TlsSessionMode::stateful) {
    SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_SERVER);
    SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
  } else {
    SSL_CTX_set_session_cache_mode(context, SSL_SESS_CACHE_SERVER | SSL_SESS_CACHE_NO_INTERNAL);
    SSL_CTX_clear_options(context, SSL_OP_NO_TICKET);
  }
  const auto tickets = options.mode == TlsSessionMode::disabled ? 0 : options.tickets;
  if (SSL_CTX_set_num_tickets(context, tickets) != 1)
    return std::unexpected(tls_native_error());

  std::array<unsigned char, 32> identity;
  if (RAND_bytes(identity.data(), static_cast<int>(identity.size())) != 1 ||
    SSL_CTX_set_session_id_context(context, identity.data(), static_cast<unsigned int>(identity.size())) != 1)
    return std::unexpected(tls_native_error());
  return {};
}

Result<void> detail::tls_verify_chain(SSL *ssl, const TlsCredentials &credentials, std::span<X509 *const> saved)
{
  ERR_clear_error();
  auto *leaf = SSL_get0_peer_certificate(ssl);
  if (!leaf) {
    if (credentials.server && credentials.client_auth != TlsClientAuth::required)
      return {};
    return std::unexpected(make_error_code(TlsError::certificate_verification));
  }
  if (credentials.server && credentials.client_auth == TlsClientAuth::none)
    return {};
  if (!credentials.server && credentials.verification == TlsVerification::none)
    return {};

  auto stack_free = [](STACK_OF(X509) *stack) {
    sk_X509_free(stack);
  };
  std::unique_ptr<STACK_OF(X509), decltype(stack_free)> chain{sk_X509_new_null(), stack_free};
  Native<X509_STORE_CTX, X509_STORE_CTX_free> verifier{X509_STORE_CTX_new(), X509_STORE_CTX_free};
  if (!chain || !verifier)
    return std::unexpected(std::make_error_code(std::errc::not_enough_memory));

  if (saved.empty()) {
    auto *native = SSL_get0_verified_chain(ssl);
    if (!native)
      native = SSL_get_peer_cert_chain(ssl);
    const auto count = native ? sk_X509_num(native) : 0;
    for (int i = 0; i < count; ++i) {
      if (sk_X509_push(chain.get(), sk_X509_value(native, i)) == 0)
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
  } else {
    for (auto *certificate : saved) {
      if (sk_X509_push(chain.get(), certificate) == 0)
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }
  }
  if (X509_STORE_CTX_init(verifier.get(), SSL_CTX_get_cert_store(credentials.context), leaf, chain.get()) != 1 ||
    X509_VERIFY_PARAM_set1(X509_STORE_CTX_get0_param(verifier.get()), SSL_get0_param(ssl)) != 1 ||
    X509_STORE_CTX_set_purpose(
      verifier.get(),
      credentials.server ? X509_PURPOSE_SSL_CLIENT : X509_PURPOSE_SSL_SERVER) != 1)
    return std::unexpected(tls_native_error());
  if (X509_verify_cert(verifier.get()) != 1)
    return std::unexpected(make_error_code(TlsError::certificate_verification));
  return {};
}

Result<void> detail::tls_verify_ocsp(SSL *ssl, const TlsCredentials &credentials)
{
  ERR_clear_error();
  if (credentials.ocsp == TlsOcsp::none)
    return {};
  const unsigned char *encoded = nullptr;
  const auto size = SSL_get_tlsext_status_ocsp_resp(ssl, &encoded);
  if (size <= 0)
    return credentials.ocsp == TlsOcsp::required ? revoked() : Result<void>{};
  if (size > 16384 || !encoded)
    return revoked();

  auto outer = response({encoded, static_cast<std::size_t>(size)});
  if (!outer)
    return revoked();
  Native<OCSP_BASICRESP, OCSP_BASICRESP_free> basic{OCSP_response_get1_basic(outer.get()), OCSP_BASICRESP_free};
  auto *chain = SSL_get0_verified_chain(ssl);
  auto *leaf = SSL_get0_peer_certificate(ssl);
  if (!basic || !chain || !leaf ||
    OCSP_basic_verify(basic.get(), chain, SSL_CTX_get_cert_store(credentials.context), 0) != 1)
    return revoked();

  auto *issuer = sk_X509_num(chain) > 1 ? sk_X509_value(chain, 1) : leaf;
  Native<OCSP_CERTID, OCSP_CERTID_free> id{OCSP_cert_to_id(nullptr, leaf, issuer), OCSP_CERTID_free};
  int status = V_OCSP_CERTSTATUS_UNKNOWN;
  int reason = 0;
  ASN1_GENERALIZEDTIME *revoked_at = nullptr;
  ASN1_GENERALIZEDTIME *updated = nullptr;
  ASN1_GENERALIZEDTIME *expires = nullptr;
  if (!id || OCSP_resp_find_status(basic.get(), id.get(), &status, &reason, &revoked_at, &updated, &expires) != 1 ||
    status != V_OCSP_CERTSTATUS_GOOD || !expires ||
    OCSP_check_validity(
      updated,
      expires,
      static_cast<long>(credentials.ocsp_clock_skew.count()),
      static_cast<long>(credentials.ocsp_max_age.count())) != 1)
    return revoked();
  return {};
}

bool detail::tls_staple_ready(const TlsCredentials &credentials) noexcept
{
  auto outer = response(credentials.ocsp_response);
  if (!outer)
    return false;
  Native<OCSP_BASICRESP, OCSP_BASICRESP_free> basic{OCSP_response_get1_basic(outer.get()), OCSP_BASICRESP_free};
  if (!basic)
    return false;
  auto *certificate = SSL_CTX_get0_certificate(credentials.context);
  const auto count = OCSP_resp_count(basic.get());
  if (count < 1 || count > 16)
    return false;
  for (int i = 0; i < count; ++i) {
    auto *single = OCSP_resp_get0(basic.get(), i);
    auto *id = OCSP_SINGLERESP_get0_id(single);
    ASN1_INTEGER *serial = nullptr;
    ASN1_GENERALIZEDTIME *updated = nullptr;
    ASN1_GENERALIZEDTIME *expires = nullptr;
    int reason = 0;
    ASN1_GENERALIZEDTIME *revoked_at = nullptr;
    if (OCSP_id_get0_info(nullptr, nullptr, nullptr, &serial, const_cast<OCSP_CERTID *>(id)) != 1 ||
      ASN1_INTEGER_cmp(serial, X509_get0_serialNumber(certificate)) != 0)
      continue;
    return OCSP_single_get0_status(single, &reason, &revoked_at, &updated, &expires) == V_OCSP_CERTSTATUS_GOOD &&
      expires && OCSP_check_validity(updated, expires, 60, 3600) == 1;
  }
  return false;
}

} // namespace weave
