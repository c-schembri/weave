#include <weave/tls/detail/engine.hpp>
#include <weave/address.hpp>
#include <weave/io/detail/execution.hpp>
#include "trust.hpp"
#include "credentials.hpp"
#include "key_log.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/store.h>
#include <openssl/ui.h>
#include <algorithm>
#include <array>
#include <mutex>

namespace weave {

struct TlsPasswordProvider::Impl {
  Handler handler;

  explicit Impl(Handler callback) noexcept : handler(std::move(callback))
  {
  }
};

// Named friend access keeps invocation private without exposing native loader state.
struct detail::TlsPasswordAccess {
  static bool valid(const TlsPasswordProvider &provider) noexcept
  {
    return provider.impl_ != nullptr;
  }

  static Result<std::string> request(const TlsPasswordProvider &provider, std::string_view file) noexcept
  {
    return provider.impl_->handler(file);
  }
};

namespace {

struct PasswordCleanup {
  std::string &password;
  char *data = password.data();
  std::size_t size = password.size();

  ~PasswordCleanup()
  {
    if (password.data() == data && password.size() < size && size <= password.capacity())
      password.resize(size);
    OPENSSL_cleanse(password.data(), password.size());
    password.clear();
  }
};

int no_password(char *buffer, int size, int, void *) noexcept
{
  if (size > 0)
    buffer[0] = '\0';
  return 0;
}

struct PasswordLoad {
  SSL_CTX *context;
  const std::string &password;
  const std::optional<TlsPasswordProvider> &provider;
  std::string_view file;
  std::optional<Error> error;

  int copy(char *buffer, int size, std::string_view secret) noexcept
  {
    if (size <= 0 || secret.size() >= static_cast<std::size_t>(size)) {
      error = make_error_code(TlsError::resource_limit);
      return -1;
    }

    std::copy(secret.begin(), secret.end(), buffer);
    buffer[secret.size()] = '\0';
    return static_cast<int>(secret.size());
  }

  static int read(char *buffer, int size, int, void *state) noexcept
  {
    auto &load = *static_cast<PasswordLoad *>(state);
    if (size > 0)
      buffer[0] = '\0';
    if (load.error)
      return -1;
    if (!load.provider)
      return load.copy(buffer, size, load.password);

    auto secret = detail::TlsPasswordAccess::request(*load.provider, load.file);
    if (!secret) {
      load.error = secret.error();
      return -1;
    }

    PasswordCleanup cleanup{*secret};
    return load.copy(buffer, size, *secret);
  }

  ~PasswordLoad()
  {
    // An immutable credential context must never retain this stack frame or provider.
    SSL_CTX_set_default_passwd_cb(context, no_password);
    SSL_CTX_set_default_passwd_cb_userdata(context, nullptr);
  }
};

bool valid_password(const std::string &password, const std::optional<TlsPasswordProvider> &provider)
{
  return !provider || (password.empty() && detail::TlsPasswordAccess::valid(*provider));
}

class TlsCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.tls";
  }

  std::string message(int value) const override
  {
    switch (static_cast<TlsError>(value)) {
    case TlsError::protocol:
      return "TLS protocol failure";
    case TlsError::certificate_verification:
      return "TLS certificate or hostname verification failed";
    case TlsError::truncated:
      return "TLS transport ended without close_notify";
    case TlsError::closed:
      return "TLS stream closed";
    case TlsError::resource_limit:
      return "TLS resource limit exceeded";
    case TlsError::session_unavailable:
      return "No resumable TLS session is available";
    case TlsError::session_rejected:
      return "TLS session is expired, already offered or belongs to another identity/policy";
    case TlsError::revocation:
      return "TLS revocation evidence is missing, invalid or reports a revoked certificate";
    case TlsError::client_certificate_required:
      return "TLS peer did not negotiate the required client certificate";
    }

    return "Unknown TLS error";
  }
};

class OpenSslCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "openssl";
  }

  std::string message(int value) const override
  {
    std::array<char, 256> text{};
    ERR_error_string_n(static_cast<unsigned int>(value), text.data(), text.size());
    return text.data();
  }
};

Error openssl_error() noexcept
{
  const auto code = ERR_peek_last_error();
  if (!code)
    return make_error_code(TlsError::protocol);

  if (ERR_GET_LIB(code) == ERR_LIB_SSL && ERR_GET_REASON(code) == SSL_R_UNEXPECTED_EOF_WHILE_READING)
    return make_error_code(TlsError::truncated);

  static OpenSslCategory category;

  return {static_cast<int>(code), category};
}

Result<TlsPeerIdentity> certificate_identity(X509 *certificate, std::size_t limit)
{
  ERR_clear_error();
  const auto size = i2d_X509(certificate, nullptr);
  if (size <= 0)
    return std::unexpected(openssl_error());
  if (static_cast<std::size_t>(size) > limit)
    return std::unexpected(make_error_code(TlsError::resource_limit));

  TlsPeerIdentity identity;
  identity.certificate.resize(static_cast<std::size_t>(size));
  auto *encoded = reinterpret_cast<unsigned char *>(identity.certificate.data());
  if (i2d_X509(certificate, &encoded) != size)
    return std::unexpected(openssl_error());

  const auto name = [](X509_NAME *value) -> Result<std::string> {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio{BIO_new(BIO_s_mem()), BIO_free};
    if (!bio || X509_NAME_print_ex(bio.get(), value, 0, XN_FLAG_RFC2253) < 0)
      return std::unexpected(openssl_error());
    char *text = nullptr;
    const auto length = BIO_get_mem_data(bio.get(), &text);
    if (length < 0)
      return std::unexpected(openssl_error());
    return length ? std::string{text, static_cast<std::size_t>(length)} : std::string{};
  };
  auto subject = name(X509_get_subject_name(certificate));
  if (!subject)
    return std::unexpected(subject.error());
  auto issuer = name(X509_get_issuer_name(certificate));
  if (!issuer)
    return std::unexpected(issuer.error());
  identity.subject = std::move(*subject);
  identity.issuer = std::move(*issuer);
  identity.sha256.resize(32);
  unsigned int digest_size = 0;
  if (X509_digest(certificate, EVP_sha256(), reinterpret_cast<unsigned char *>(identity.sha256.data()), &digest_size) !=
    1)
    return std::unexpected(openssl_error());
  identity.sha256.resize(digest_size);

  return identity;
}

int protocol(TlsVersion version) noexcept
{
  switch (version) {
  case TlsVersion::tls12:
    return TLS1_2_VERSION;
  case TlsVersion::tls13:
    return TLS1_3_VERSION;
  }

  return 0;
}

Result<std::vector<unsigned char>> encode_alpn(const std::vector<std::string> &protocols)
{
  std::vector<unsigned char> encoded;

  for (const auto &name : protocols) {
    if (name.empty() || name.size() > 255 || encoded.size() + name.size() + 1 > 65535)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    encoded.push_back(static_cast<unsigned char>(name.size()));
    encoded.insert(encoded.end(), name.begin(), name.end());
  }

  return encoded;
}

Result<std::shared_ptr<detail::TlsCredentials>> credentials(
  bool server,
  TlsVersion minimum,
  TlsVersion maximum,
  const std::vector<std::string> &protocols,
  TlsLimits limits,
  const TlsCipherPolicy &ciphers)
{
  if (!protocol(minimum) || !protocol(maximum) || protocol(minimum) > protocol(maximum))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  constexpr std::size_t maximum_buffer = 16 * 1024 * 1024;
  const bool invalid_limits = limits.buffered_input < 32768 || limits.buffered_output < 32768 ||
    limits.certificate_chain < 4096 || limits.buffered_input > maximum_buffer ||
    limits.buffered_output > maximum_buffer || limits.certificate_chain > limits.buffered_input ||
    limits.certificate_chain > limits.buffered_output || limits.verification_depth < 1 ||
    limits.verification_depth > 64;
  const std::array strings{&ciphers.tls12, &ciphers.tls13, &ciphers.groups, &ciphers.signatures};
  const bool nul = std::any_of(strings.begin(), strings.end(), [](const std::string *value) {
    return value->find('\0') != std::string::npos;
  });
  if (invalid_limits || nul || ciphers.security_level < 2 || ciphers.security_level > 5)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto alpn = encode_alpn(protocols);
  if (!alpn)
    return std::unexpected(alpn.error());

  auto value = std::make_shared<detail::TlsCredentials>();
  value->server = server;
  value->alpn = std::move(*alpn);
  value->limits = limits;

  ERR_clear_error();
  value->context = SSL_CTX_new(TLS_method());
  if (!value->context)
    return std::unexpected(openssl_error());

  if (!SSL_CTX_set_min_proto_version(value->context, protocol(minimum)) ||
    !SSL_CTX_set_max_proto_version(value->context, protocol(maximum)))
    return std::unexpected(openssl_error());

  SSL_CTX_set_options(value->context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
  SSL_CTX_set_security_level(value->context, ciphers.security_level);
  SSL_CTX_set_verify_depth(value->context, limits.verification_depth);
  SSL_CTX_set_max_cert_list(value->context, static_cast<long>(limits.certificate_chain));
  SSL_CTX_set_max_early_data(value->context, 0);

  const auto *tls12 = ciphers.tls12.empty() ? "ECDHE+AESGCM:ECDHE+CHACHA20" : ciphers.tls12.c_str();
  if ((SSL_CTX_set_cipher_list(value->context, tls12) != 1) ||
    (!ciphers.tls13.empty() && SSL_CTX_set_ciphersuites(value->context, ciphers.tls13.c_str()) != 1) ||
    (!ciphers.groups.empty() && SSL_CTX_set1_groups_list(value->context, ciphers.groups.c_str()) != 1) ||
    (!ciphers.signatures.empty() && SSL_CTX_set1_sigalgs_list(value->context, ciphers.signatures.c_str()) != 1))
    return std::unexpected(openssl_error());

  // Loading an encrypted key must never prompt on stdin.
  SSL_CTX_set_default_passwd_cb(value->context, no_password);

  if (server && !value->alpn.empty()) {
    SSL_CTX_set_alpn_select_cb(
      value->context,
      [](
        SSL *,
        const unsigned char **out,
        unsigned char *length,
        const unsigned char *input,
        unsigned int size,
        void *state) {
        const auto &list = static_cast<detail::TlsCredentials *>(state)->alpn;
        unsigned char *selected = nullptr;

        auto result = SSL_select_next_proto(
          &selected,
          length,
          list.data(),
          static_cast<unsigned int>(list.size()),
          input,
          size);

        *out = selected;

        return result == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
      },
      value.get());
  }

  return value;
}

bool valid_paths(std::initializer_list<std::string *> paths)
{
  return std::all_of(paths.begin(), paths.end(), [](const std::string *path) {
    return path->find('\0') == std::string::npos;
  });
}

Result<void> load_identity(
  detail::TlsCredentials &value,
  const std::string &certificate,
  const std::string &key,
  const std::string &password,
  const std::optional<TlsPasswordProvider> &provider,
  TlsPrivateKeyFormat format)
{
  if (format < TlsPrivateKeyFormat::pem || format > TlsPrivateKeyFormat::store)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (certificate.empty() && key.empty())
    return {};
  if (certificate.empty() || key.empty())
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  ERR_clear_error();
  if (SSL_CTX_use_certificate_chain_file(value.context, certificate.c_str()) != 1)
    return std::unexpected(openssl_error());

  PasswordLoad load{value.context, password, provider, key, {}};
  SSL_CTX_set_default_passwd_cb(value.context, PasswordLoad::read);
  SSL_CTX_set_default_passwd_cb_userdata(value.context, &load);
  int loaded = 0;
  if (format == TlsPrivateKeyFormat::store) {
    std::unique_ptr<UI_METHOD, decltype(&UI_destroy_method)> ui{
      UI_UTIL_wrap_read_pem_callback(PasswordLoad::read, 0),
      UI_destroy_method};
    if (!ui)
      return std::unexpected(openssl_error());

    std::unique_ptr<OSSL_STORE_CTX, decltype(&OSSL_STORE_close)> store{
      OSSL_STORE_open_ex(key.c_str(), nullptr, nullptr, ui.get(), &load, nullptr, nullptr, nullptr),
      OSSL_STORE_close};
    if (!store || OSSL_STORE_expect(store.get(), OSSL_STORE_INFO_PKEY) != 1)
      return std::unexpected(load.error ? *load.error : openssl_error());

    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> identity{nullptr, EVP_PKEY_free};
    std::size_t objects = 0;
    while (!OSSL_STORE_eof(store.get())) {
      if (++objects > 64)
        return std::unexpected(make_error_code(TlsError::resource_limit));
      ERR_clear_error();
      std::unique_ptr<OSSL_STORE_INFO, decltype(&OSSL_STORE_INFO_free)> item{
        OSSL_STORE_load(store.get()),
        OSSL_STORE_INFO_free};
      if (load.error)
        return std::unexpected(*load.error);
      if (!item) {
        // OpenSSL's file loader can set its error flag at clean EOF without an error record.
        if (!OSSL_STORE_eof(store.get()) || (OSSL_STORE_error(store.get()) && ERR_peek_last_error()))
          return std::unexpected(openssl_error());
        break;
      }
      if (OSSL_STORE_INFO_get_type(item.get()) != OSSL_STORE_INFO_PKEY)
        continue;
      if (identity)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
      identity.reset(OSSL_STORE_INFO_get1_PKEY(item.get()));
      if (!identity)
        return std::unexpected(openssl_error());
    }
    if (!identity)
      return std::unexpected(make_error_code(TlsError::protocol));
    loaded = SSL_CTX_use_PrivateKey(value.context, identity.get());
  } else {
    const auto type = format == TlsPrivateKeyFormat::pem ? SSL_FILETYPE_PEM : SSL_FILETYPE_ASN1;
    loaded = SSL_CTX_use_PrivateKey_file(value.context, key.c_str(), type);
  }
  if (load.error)
    return std::unexpected(*load.error);
  if (loaded != 1 || SSL_CTX_check_private_key(value.context) != 1)
    return std::unexpected(openssl_error());

  STACK_OF(X509) *chain = nullptr;
  SSL_CTX_get0_chain_certs(value.context, &chain);
  const auto count = chain ? sk_X509_num(chain) : 0;
  const auto leaf_size = i2d_X509(SSL_CTX_get0_certificate(value.context), nullptr);
  if (leaf_size <= 0)
    return std::unexpected(openssl_error());
  std::size_t size = static_cast<std::size_t>(leaf_size);
  for (int i = 0; i < count; ++i) {
    const auto encoded = i2d_X509(sk_X509_value(chain, i), nullptr);
    if (encoded <= 0)
      return std::unexpected(openssl_error());
    size += static_cast<std::size_t>(encoded);
  }
  if (count > value.limits.verification_depth || size > value.limits.certificate_chain)
    return std::unexpected(make_error_code(TlsError::resource_limit));

  return {};
}

Result<void> load_trust(
  detail::TlsCredentials &value,
  const std::string &file,
  const std::string &directory,
  const std::string &crl,
  const std::string &crl_directory,
  TlsRevocation revocation)
{
  if (revocation != TlsRevocation::none && revocation != TlsRevocation::leaf && revocation != TlsRevocation::chain)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if ((revocation == TlsRevocation::none) != (crl.empty() && crl_directory.empty()))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto *store = SSL_CTX_get_cert_store(value.context);
  ERR_clear_error();
  if (!file.empty() || !directory.empty()) {
    const auto *ca = file.empty() ? nullptr : file.c_str();
    const auto *path = directory.empty() ? nullptr : directory.c_str();
    if (SSL_CTX_load_verify_locations(value.context, ca, path) != 1)
      return std::unexpected(openssl_error());
  } else {
    const bool defaults = SSL_CTX_set_default_verify_paths(value.context) == 1;
    const bool system = detail::system_roots(store);
    if (!defaults && !system)
      return std::unexpected(openssl_error());
  }

  if (revocation != TlsRevocation::none) {
    if (!crl.empty()) {
      auto *lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
      if (!lookup || X509_load_crl_file(lookup, crl.c_str(), X509_FILETYPE_PEM) <= 0)
        return std::unexpected(openssl_error());
    }
    if (!crl_directory.empty()) {
      auto *lookup = X509_STORE_add_lookup(store, X509_LOOKUP_hash_dir());
      if (!lookup || X509_LOOKUP_add_dir(lookup, crl_directory.c_str(), X509_FILETYPE_PEM) != 1)
        return std::unexpected(openssl_error());
    }
    const auto flags = X509_V_FLAG_CRL_CHECK | (revocation == TlsRevocation::chain ? X509_V_FLAG_CRL_CHECK_ALL : 0);
    if (X509_STORE_set_flags(store, flags) != 1)
      return std::unexpected(openssl_error());
  }
  return {};
}

} // namespace

TlsPasswordProvider::TlsPasswordProvider(std::shared_ptr<const Impl> impl) noexcept : impl_(std::move(impl))
{
}

Result<TlsPasswordProvider> TlsPasswordProvider::create(Handler handler)
{
  if (!handler)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return TlsPasswordProvider{std::make_shared<Impl>(std::move(handler))};
}

Error make_error_code(TlsError error) noexcept
{
  static TlsCategory category;

  return {static_cast<int>(error), category};
}

Error detail::tls_native_error() noexcept
{
  return openssl_error();
}

Result<TlsContext> TlsContext::client(TlsClientOptions options)
{
  PasswordCleanup cleanup{options.private_key_password};
  std::optional<PasswordCleanup> locator_cleanup;
  if (options.private_key_format == TlsPrivateKeyFormat::store)
    locator_cleanup.emplace(options.private_key_file);
  const auto paths = {
    &options.ca_file,
    &options.ca_directory,
    &options.crl_file,
    &options.crl_directory,
    &options.certificate_file,
    &options.private_key_file};
  const bool invalid_ocsp = options.ocsp != TlsOcsp::none && options.ocsp != TlsOcsp::if_present &&
    options.ocsp != TlsOcsp::required;
  const bool invalid_freshness = options.ocsp_max_age < std::chrono::seconds{1} ||
    options.ocsp_max_age > std::chrono::hours{24} || options.ocsp_clock_skew < std::chrono::seconds{0} ||
    options.ocsp_clock_skew > std::chrono::minutes{10};
  const bool invalid_sessions = options.session_lifetime < std::chrono::seconds{1} ||
    options.session_lifetime > std::chrono::hours{24} || (options.ocsp != TlsOcsp::none && options.session_resumption);
  const bool unused_password = options.private_key_file.empty() &&
    (!options.private_key_password.empty() || options.private_key_password_provider.has_value());
  const bool invalid_password = !valid_password(options.private_key_password, options.private_key_password_provider);
  const bool invalid_verification = options.verification < TlsVerification::none ||
    options.verification > TlsVerification::hostname ||
    (options.verification == TlsVerification::none &&
      (options.revocation != TlsRevocation::none || options.ocsp != TlsOcsp::none || options.session_resumption));
  if (!valid_paths(paths) || invalid_ocsp || invalid_freshness || invalid_sessions || unused_password ||
    invalid_password || invalid_verification)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto
    value = credentials(false, options.min_version, options.max_version, options.alpn, options.limits, options.ciphers);
  if (!value)
    return std::unexpected(value.error());

  auto *native = (*value)->context;
  (*value)->verification = options.verification;
  SSL_CTX_set_verify(
    native,
    options.verification == TlsVerification::none ? SSL_VERIFY_NONE : SSL_VERIFY_PEER,
    nullptr);
  (*value)->ocsp = options.ocsp;
  (*value)->ocsp_max_age = options.ocsp_max_age;
  (*value)->ocsp_clock_skew = options.ocsp_clock_skew;
  (*value)->resumption = options.session_resumption;
  (*value)->session_lifetime = options.session_lifetime;
  if (options.verification != TlsVerification::none) {
    if (auto status = load_trust(
          **value,
          options.ca_file,
          options.ca_directory,
          options.crl_file,
          options.crl_directory,
          options.revocation);
      !status)
      return std::unexpected(status.error());
  }
  if (auto status = load_identity(
        **value,
        options.certificate_file,
        options.private_key_file,
        options.private_key_password,
        options.private_key_password_provider,
        options.private_key_format);
    !status)
    return std::unexpected(status.error());

  const auto cache = options.session_resumption ? SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL
                                                : SSL_SESS_CACHE_OFF;
  SSL_CTX_set_session_cache_mode(native, cache);
  if (!options.session_resumption)
    SSL_CTX_set_options(native, SSL_OP_NO_TICKET);

  if (auto status = detail::tls_configure_key_log(**value, options.key_log_file); !status)
    return std::unexpected(status.error());

  return TlsContext{std::move(*value)};
}

Result<TlsContext> TlsContext::server(TlsServerOptions options)
{
  PasswordCleanup cleanup{options.private_key_password};
  std::optional<PasswordCleanup> locator_cleanup;
  if (options.private_key_format == TlsPrivateKeyFormat::store)
    locator_cleanup.emplace(options.private_key_file);
  const auto paths = {
    &options.certificate_file,
    &options.private_key_file,
    &options.ca_file,
    &options.ca_directory,
    &options.crl_file,
    &options.crl_directory,
    &options.ocsp_file};
  const bool invalid_auth = options.client_auth != TlsClientAuth::none &&
    options.client_auth != TlsClientAuth::optional && options.client_auth != TlsClientAuth::required;
  const bool missing_trust = options.client_auth != TlsClientAuth::none && options.ca_file.empty() &&
    options.ca_directory.empty();
  const bool unused_trust = options.client_auth == TlsClientAuth::none &&
    (options.revocation != TlsRevocation::none || !options.crl_file.empty() || !options.crl_directory.empty() ||
      !options.ca_file.empty() || !options.ca_directory.empty());
  const bool invalid_password = !valid_password(options.private_key_password, options.private_key_password_provider);
  if (options.certificate_file.empty() || options.private_key_file.empty() || !valid_paths(paths) || invalid_auth ||
    missing_trust || unused_trust || invalid_password)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto
    value = credentials(true, options.min_version, options.max_version, options.alpn, options.limits, options.ciphers);
  if (!value)
    return std::unexpected(value.error());

  (*value)->client_auth = options.client_auth;
  (*value)->verification = options.client_auth == TlsClientAuth::none ? TlsVerification::none
                                                                      : TlsVerification::certificate;

  auto *native = (*value)->context;
  if (options.client_auth != TlsClientAuth::none) {
    if (auto status = load_trust(
          **value,
          options.ca_file,
          options.ca_directory,
          options.crl_file,
          options.crl_directory,
          options.revocation);
      !status)
      return std::unexpected(status.error());
    const auto flags = SSL_VERIFY_PEER |
      (options.client_auth == TlsClientAuth::required ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0);
    SSL_CTX_set_verify(native, flags, nullptr);
    if (!options.ca_file.empty()) {
      auto *names = SSL_load_client_CA_file(options.ca_file.c_str());
      if (!names)
        return std::unexpected(openssl_error());
      SSL_CTX_set_client_CA_list(native, names);
    }
  }
  if (auto status = detail::tls_configure_sessions(**value, options.sessions); !status)
    return std::unexpected(status.error());

  if (auto status = load_identity(
        **value,
        options.certificate_file,
        options.private_key_file,
        options.private_key_password,
        options.private_key_password_provider,
        options.private_key_format);
    !status)
    return std::unexpected(status.error());

  if (!options.ocsp_file.empty()) {
    auto *file = BIO_new_file(options.ocsp_file.c_str(), "rb");
    if (!file)
      return std::unexpected(openssl_error());
    std::array<unsigned char, 16384> buffer;
    const auto count = BIO_read(file, buffer.data(), static_cast<int>(buffer.size()));
    unsigned char extra;
    const auto trailing = BIO_read(file, &extra, 1);
    BIO_free(file);
    if (count <= 0 || trailing != 0)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    (*value)->ocsp_response.assign(buffer.begin(), buffer.begin() + count);
    if (!detail::tls_staple_ready(**value))
      return std::unexpected(make_error_code(TlsError::revocation));
    const auto staple = +[](SSL *ssl, void *state) -> int {
      const auto &credentials = *static_cast<detail::TlsCredentials *>(state);
      if (!detail::tls_staple_ready(credentials))
        return SSL_TLSEXT_ERR_ALERT_FATAL;
      const auto &response = credentials.ocsp_response;
      auto *copy = static_cast<unsigned char *>(OPENSSL_memdup(response.data(), response.size()));
      if (!copy)
        return SSL_TLSEXT_ERR_ALERT_FATAL;
      if (SSL_set_tlsext_status_ocsp_resp(ssl, copy, static_cast<int>(response.size())) != 1) {
        OPENSSL_free(copy);
        return SSL_TLSEXT_ERR_ALERT_FATAL;
      }
      return SSL_TLSEXT_ERR_OK;
    };
    SSL_CTX_set_tlsext_status_cb(native, staple);
    SSL_CTX_set_tlsext_status_arg(native, value->get());
  }

  if (auto status = detail::tls_configure_key_log(**value, options.key_log_file); !status)
    return std::unexpected(status.error());

  return TlsContext{std::move(*value)};
}

struct detail::TlsEngine::Impl {
  std::shared_ptr<TlsCredentials> credentials;
  SSL *ssl = nullptr;

  mutable std::mutex mutex;
  Error error;
  bool reading = false;
  bool writing = false;
  bool exclusive = false;

  bool input_eof = false;
  bool verified = false;
  std::string name;
  std::string required_protocol;
  bool server_name_indication = true;
  TlsCertificateMode client_certificate = TlsCertificateMode::allow;
  bool certificate_requested = false;
  bool certificate_proof = false;
  bool client_certificate_used = false;
  std::shared_ptr<TlsSessionState> captured;
  SSL_SESSION *captured_source = nullptr;
  std::shared_ptr<TlsSessionState> resumed;
  u64 generation = 0;

  static void observe_client_identity(
    int writing,
    int,
    int content,
    const void *buffer,
    std::size_t size,
    SSL *,
    void *argument) noexcept
  {
    if (content != SSL3_RT_HANDSHAKE || size == 0)
      return;
    auto &self = *static_cast<Impl *>(argument);
    const auto type = *static_cast<const unsigned char *>(buffer);
    if (!writing && type == SSL3_MT_CERTIFICATE_REQUEST)
      self.certificate_requested = true;
    if (writing && type == SSL3_MT_CERTIFICATE_VERIFY)
      self.certificate_proof = true;
  }

  bool &flag(TlsOperation operation) noexcept
  {
    if (operation == TlsOperation::read)
      return reading;
    if (operation == TlsOperation::write)
      return writing;

    return exclusive;
  }

  void revoke_session() noexcept
  {
    if (captured)
      captured->revoked.store(true, std::memory_order_release);
  }

  ~Impl()
  {
    if (ssl && !(SSL_get_shutdown(ssl) & SSL_SENT_SHUTDOWN))
      revoke_session();
    SSL_SESSION_free(captured_source);
    if (ssl)
      SSL_free(ssl);
  }

  TlsStep step(int result, std::size_t transferred = 0) noexcept
  {
    // Capture SSL_get_error before any intervening OpenSSL/BIO calls.
    const auto code = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl, result);
    const auto native_error = result == 1 ? Error{} : openssl_error();
    if (auto logging = tls_key_log_error(*credentials); logging) {
      error = logging;
      revoke_session();
      return {TlsAction::failed, 0, error, generation};
    }
    if (BIO_ctrl_pending(SSL_get_wbio(ssl)) > credentials->limits.buffered_output) {
      error = make_error_code(TlsError::resource_limit);
      revoke_session();
      return {TlsAction::failed, 0, error, generation};
    }
    if (result == 1)
      return {TlsAction::ready, transferred, {}, generation};

    if (code == SSL_ERROR_WANT_READ && !input_eof)
      return {TlsAction::input, 0, {}, generation};
    if (code == SSL_ERROR_WANT_WRITE)
      return {TlsAction::output, 0, {}, generation};
    if (code == SSL_ERROR_ZERO_RETURN)
      return {TlsAction::eof, 0, {}, generation};

    if ((credentials->server || credentials->verification != TlsVerification::none) &&
      SSL_get_verify_result(ssl) != X509_V_OK)
      error = make_error_code(TlsError::certificate_verification);
    else
      error = native_error;

    if (code == SSL_ERROR_WANT_READ && input_eof)
      error = make_error_code(TlsError::truncated);

    revoke_session();

    return {TlsAction::failed, 0, error, generation};
  }
};

Result<detail::TlsEngine> detail::TlsEngine::create(
  const TlsContext &context,
  bool server,
  const std::string &name,
  TlsSession *session,
  std::string_view required_protocol,
  bool server_name_indication,
  TlsCertificateMode client_certificate)
{
  if (!context.credentials_ || context.credentials_->server != server || required_protocol.size() > 255 ||
    (!server && (name.empty() || name.find('\0') != std::string::npos)) ||
    client_certificate < TlsCertificateMode::disable || client_certificate > TlsCertificateMode::require)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto impl = std::make_unique<Impl>();
  impl->credentials = context.credentials_;
  impl->name = name;
  impl->required_protocol = required_protocol;
  impl->server_name_indication = server_name_indication;
  impl->client_certificate = client_certificate;

  ERR_clear_error();
  impl->ssl = SSL_new(context.credentials_->context);
  if (!impl->ssl)
    return std::unexpected(openssl_error());

  auto *input = BIO_new(BIO_s_mem());
  auto *output = BIO_new(BIO_s_mem());
  if (!input || !output) {
    BIO_free(input);
    BIO_free(output);
    return std::unexpected(openssl_error());
  }

  BIO_set_mem_eof_return(input, -1);
  BIO_set_mem_eof_return(output, -1);
  SSL_set_bio(impl->ssl, input, output);

  if (server)
    SSL_set_accept_state(impl->ssl);
  else {
    SSL_set_connect_state(impl->ssl);
    if (client_certificate == TlsCertificateMode::disable)
      SSL_certs_clear(impl->ssl);
    if (client_certificate == TlsCertificateMode::require) {
      // Observe OpenSSL's native messages, not configured identity or an attempted selection.
      SSL_set_msg_callback(impl->ssl, Impl::observe_client_identity);
      SSL_set_msg_callback_arg(impl->ssl, impl.get());
    }
    SSL_set_hostflags(impl->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);

    auto ip = IpAddress::parse(name);
    const bool verify_name = impl->credentials->verification == TlsVerification::hostname;
    if (ip && verify_name) {
      if (!X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(impl->ssl), name.c_str()))
        return std::unexpected(openssl_error());
    } else if (!ip) {
      if (verify_name && !SSL_set1_host(impl->ssl, name.c_str()))
        return std::unexpected(openssl_error());
      if (server_name_indication && !SSL_set_tlsext_host_name(impl->ssl, name.c_str()))
        return std::unexpected(openssl_error());
    }

    if (!required_protocol.empty()) {
      std::array<unsigned char, 256> alpn{};
      alpn.front() = static_cast<unsigned char>(required_protocol.size());
      std::copy(required_protocol.begin(), required_protocol.end(), alpn.begin() + 1);
      if (SSL_set_alpn_protos(impl->ssl, alpn.data(), static_cast<unsigned int>(required_protocol.size() + 1)) != 0)
        return std::unexpected(openssl_error());
    } else {
      const auto &alpn = impl->credentials->alpn;
      if (!alpn.empty() && SSL_set_alpn_protos(impl->ssl, alpn.data(), static_cast<unsigned int>(alpn.size())) != 0)
        return std::unexpected(openssl_error());
    }

    if (impl->credentials->ocsp != TlsOcsp::none && SSL_set_tlsext_status_type(impl->ssl, TLSEXT_STATUSTYPE_ocsp) != 1)
      return std::unexpected(openssl_error());
    if (session) {
      auto state = session->state_;
      if (!state || !impl->credentials->resumption || state->credentials != impl->credentials || state->name != name ||
        state->required_protocol != required_protocol || state->server_name_indication != server_name_indication ||
        state->client_certificate != client_certificate ||
        (client_certificate == TlsCertificateMode::require && !state->client_certificate_used) ||
        !session->available() || state->offered.exchange(true, std::memory_order_acq_rel))
        return std::unexpected(make_error_code(TlsError::session_rejected));
      auto *offer = SSL_SESSION_dup(state->session);
      if (!offer)
        return std::unexpected(openssl_error());
      const auto accepted = SSL_set_session(impl->ssl, offer);
      SSL_SESSION_free(offer);
      if (accepted != 1)
        return std::unexpected(openssl_error());
      impl->resumed = std::move(state);
    }
  }

  return TlsEngine{std::move(impl)};
}

detail::TlsEngine::TlsEngine(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
{
}

detail::TlsEngine::TlsEngine(TlsEngine &&other) noexcept = default;
detail::TlsEngine::~TlsEngine() = default;

Result<void> detail::TlsEngine::begin(TlsOperation operation) noexcept
{
  require(impl_ != nullptr && current_context != nullptr);

  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);

  const bool active_io = impl_->reading || impl_->writing;
  const bool exclusive_conflict = operation == TlsOperation::exclusive && active_io;
  const bool busy = impl_->exclusive || impl_->flag(operation) || exclusive_conflict;
  if (busy)
    return std::unexpected(std::make_error_code(std::errc::operation_in_progress));

  if (operation == TlsOperation::write && (SSL_get_shutdown(impl_->ssl) & SSL_SENT_SHUTDOWN))
    return std::unexpected(make_error_code(TlsError::closed));

  impl_->flag(operation) = true;

  return {};
}

void detail::TlsEngine::end(TlsOperation operation) noexcept
{
  std::lock_guard lock(impl_->mutex);
  impl_->flag(operation) = false;
}

bool detail::TlsEngine::active() const noexcept
{
  if (!impl_)
    return false;

  std::lock_guard lock(impl_->mutex);

  return impl_->reading || impl_->writing || impl_->exclusive;
}

void detail::TlsEngine::fail(Error error) noexcept
{
  std::lock_guard lock(impl_->mutex);
  const bool graceful_close = error == TlsError::closed && (SSL_get_shutdown(impl_->ssl) & SSL_SENT_SHUTDOWN);
  if (!graceful_close)
    impl_->revoke_session();
  if (!impl_->error)
    impl_->error = error;
}

detail::TlsStep detail::TlsEngine::handshake() noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return {TlsAction::failed, 0, impl_->error};

  ERR_clear_error();
  auto step = impl_->step(SSL_do_handshake(impl_->ssl));

  if (step.action == TlsAction::ready && !impl_->verified) {
    const bool reused = SSL_session_reused(impl_->ssl) != 0;
    if (reused && impl_->resumed && impl_->resumed->revoked.load(std::memory_order_acquire)) {
      impl_->error = make_error_code(TlsError::session_rejected);
      return {TlsAction::failed, 0, impl_->error};
    }
    const auto saved = reused && impl_->resumed ? std::span<X509 *const>{impl_->resumed->chain}
                                                : std::span<X509 *const>{};
    auto verified = tls_verify_chain(impl_->ssl, *impl_->credentials, saved);
    if (verified)
      verified = tls_verify_ocsp(impl_->ssl, *impl_->credentials);
    if (!verified) {
      impl_->error = verified.error();
      return {TlsAction::failed, 0, impl_->error};
    }
    if (!impl_->required_protocol.empty()) {
      const unsigned char *selected = nullptr;
      unsigned int size = 0;
      SSL_get0_alpn_selected(impl_->ssl, &selected, &size);
      if (size != impl_->required_protocol.size() ||
        !std::equal(
          impl_->required_protocol.begin(),
          impl_->required_protocol.end(),
          reinterpret_cast<const char *>(selected))) {
        impl_->error = make_error_code(TlsError::protocol);
        impl_->revoke_session();
        return {TlsAction::failed, 0, impl_->error};
      }
    }
    if (!impl_->credentials->server && impl_->client_certificate == TlsCertificateMode::require) {
      impl_->client_certificate_used = reused && impl_->resumed
        ? impl_->resumed->client_certificate_used
        : impl_->certificate_requested && impl_->certificate_proof;
      if (!impl_->client_certificate_used) {
        impl_->error = make_error_code(TlsError::client_certificate_required);
        impl_->revoke_session();
        return {TlsAction::failed, 0, impl_->error};
      }
    }
    impl_->verified = true;
    impl_->credentials->handshakes.fetch_add(1, std::memory_order_relaxed);
    if (reused)
      impl_->credentials->resumed.fetch_add(1, std::memory_order_relaxed);
  }

  if (step.action == TlsAction::eof) {
    impl_->error = make_error_code(TlsError::closed);
    return {TlsAction::failed, 0, impl_->error};
  }

  return step;
}

detail::TlsStep detail::TlsEngine::read(std::span<std::byte> buffer) noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return {TlsAction::failed, 0, impl_->error};

  ERR_clear_error();
  std::size_t count = 0;
  const auto result = SSL_read_ex(impl_->ssl, buffer.data(), buffer.size(), &count);

  return impl_->step(result, count);
}

detail::TlsStep detail::TlsEngine::write(std::span<const std::byte> buffer) noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return {TlsAction::failed, 0, impl_->error};

  ERR_clear_error();
  std::size_t count = 0;
  const auto result = SSL_write_ex(impl_->ssl, buffer.data(), buffer.size(), &count);
  auto step = impl_->step(result, count);

  if (step.action == TlsAction::eof) {
    impl_->error = make_error_code(TlsError::closed);
    return {TlsAction::failed, 0, impl_->error};
  }

  return step;
}

detail::TlsStep detail::TlsEngine::shutdown(bool wait_peer) noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return {TlsAction::failed, 0, impl_->error};
  if (!wait_peer && (SSL_get_shutdown(impl_->ssl) & SSL_SENT_SHUTDOWN))
    return {TlsAction::ready};

  ERR_clear_error();
  const auto result = SSL_shutdown(impl_->ssl);

  // The first zero return sends close_notify without attempting peer input.
  // Flush and retry so an already-buffered notification is consumed first.
  if (result == 0)
    return {wait_peer ? TlsAction::output : TlsAction::ready, 0, {}, impl_->generation};

  auto step = impl_->step(result);
  if (step.action == TlsAction::eof)
    step.action = TlsAction::ready;

  return step;
}

Result<bool> detail::TlsEngine::has_output() const noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);

  return BIO_ctrl_pending(SSL_get_wbio(impl_->ssl)) != 0;
}

Result<std::size_t> detail::TlsEngine::output(std::span<std::byte> buffer) noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);

  auto *bio = SSL_get_wbio(impl_->ssl);
  if (BIO_ctrl_pending(bio) > impl_->credentials->limits.buffered_output) {
    impl_->error = make_error_code(TlsError::resource_limit);
    impl_->revoke_session();
    return std::unexpected(impl_->error);
  }
  if (!BIO_ctrl_pending(bio))
    return std::size_t{0};

  ERR_clear_error();
  auto count = BIO_read(bio, buffer.data(), static_cast<int>(buffer.size()));
  if (count <= 0)
    return std::unexpected(openssl_error());

  return static_cast<std::size_t>(count);
}

Result<bool> detail::TlsEngine::needs_input(u64 generation) const noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);

  return generation == impl_->generation;
}

Result<void> detail::TlsEngine::input(std::span<const std::byte> buffer) noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);

  auto *bio = SSL_get_rbio(impl_->ssl);
  if (buffer.empty()) {
    impl_->input_eof = true;
    BIO_set_mem_eof_return(bio, 0);
  } else {
    const auto limit = impl_->credentials->limits.buffered_input;
    if (buffer.size() > limit || BIO_ctrl_pending(bio) > limit - buffer.size()) {
      impl_->error = make_error_code(TlsError::resource_limit);
      impl_->revoke_session();
      return std::unexpected(impl_->error);
    }

    ERR_clear_error();
    if (BIO_write(bio, buffer.data(), static_cast<int>(buffer.size())) != static_cast<int>(buffer.size()))
      return std::unexpected(openssl_error());
  }

  ++impl_->generation;

  return {};
}

TlsVersion detail::TlsEngine::version() const noexcept
{
  std::lock_guard lock(impl_->mutex);

  return SSL_version(impl_->ssl) == TLS1_3_VERSION ? TlsVersion::tls13 : TlsVersion::tls12;
}

std::string detail::TlsEngine::alpn() const
{
  std::lock_guard lock(impl_->mutex);
  const unsigned char *protocol = nullptr;
  unsigned int length = 0;
  SSL_get0_alpn_selected(impl_->ssl, &protocol, &length);

  return length ? std::string{reinterpret_cast<const char *>(protocol), length} : std::string{};
}

std::string detail::TlsEngine::cipher() const
{
  std::lock_guard lock(impl_->mutex);
  const auto *name = SSL_get_cipher_name(impl_->ssl);
  return name ? name : "";
}

bool detail::TlsEngine::session_reused() const noexcept
{
  std::lock_guard lock(impl_->mutex);
  return impl_->verified && SSL_session_reused(impl_->ssl) != 0;
}

Result<TlsInfo> detail::TlsEngine::info() const
{
  if (!impl_)
    return std::unexpected(make_error_code(TlsError::closed));
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);
  if (!impl_->verified)
    return std::unexpected(make_error_code(TlsError::protocol));

  const auto version = SSL_version(impl_->ssl);
  const auto *cipher = SSL_get_current_cipher(impl_->ssl);
  const auto bits = SSL_CIPHER_get_bits(cipher, nullptr);
  if ((version != TLS1_2_VERSION && version != TLS1_3_VERSION) || !cipher || bits <= 0)
    return std::unexpected(make_error_code(TlsError::protocol));

  TlsInfo info;
  info.library = "OpenSSL";
  info.version = version == TLS1_3_VERSION ? TlsVersion::tls13 : TlsVersion::tls12;
  info.cipher = SSL_CIPHER_get_name(cipher);
  info.key_bits = static_cast<u32>(bits);
  info.compression = SSL_get_current_compression(impl_->ssl) != nullptr;
  const unsigned char *protocol = nullptr;
  unsigned int length = 0;
  SSL_get0_alpn_selected(impl_->ssl, &protocol, &length);
  if (length)
    info.negotiated_protocol.assign(reinterpret_cast<const char *>(protocol), length);
  info.session_reused = SSL_session_reused(impl_->ssl) != 0;

  const bool authenticated = impl_->credentials->server ? impl_->credentials->client_auth != TlsClientAuth::none
                                                        : impl_->credentials->verification != TlsVerification::none;
  info.certificate_verified = authenticated && SSL_get0_peer_certificate(impl_->ssl);
  info.hostname_verified = info.certificate_verified && !impl_->credentials->server &&
    impl_->credentials->verification == TlsVerification::hostname;
  auto *certificate = SSL_get0_peer_certificate(impl_->ssl);
  if (authenticated && certificate) {
    auto peer = certificate_identity(certificate, impl_->credentials->limits.certificate_chain);
    if (!peer)
      return std::unexpected(peer.error());
    info.peer = std::move(*peer);
  }

  return info;
}

Result<TlsSession> detail::TlsEngine::session() const
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);
  if (!impl_->verified || impl_->credentials->server || !impl_->credentials->resumption)
    return std::unexpected(make_error_code(TlsError::session_unavailable));

  auto *source = SSL_get_session(impl_->ssl);
  if (!source || !SSL_SESSION_is_resumable(source))
    return std::unexpected(make_error_code(TlsError::session_unavailable));
  if (source == impl_->captured_source && impl_->captured) {
    TlsSession session{impl_->captured};
    if (!session.available())
      return std::unexpected(make_error_code(TlsError::session_unavailable));
    return session;
  }

  ERR_clear_error();
  auto state = std::make_shared<TlsSessionState>();
  state->session = SSL_SESSION_dup(source);
  state->credentials = impl_->credentials;
  state->name = impl_->name;
  state->required_protocol = impl_->required_protocol;
  state->server_name_indication = impl_->server_name_indication;
  state->client_certificate = impl_->client_certificate;
  state->client_certificate_used = impl_->client_certificate_used;
  if (!state->session)
    return std::unexpected(openssl_error());

  const auto native_lifetime = SSL_SESSION_get_timeout(state->session);
  const auto policy_lifetime = static_cast<long>(impl_->credentials->session_lifetime.count());
  SSL_SESSION_set_timeout(state->session, (std::min)(native_lifetime, policy_lifetime));

  auto *chain = SSL_get0_verified_chain(impl_->ssl);
  if (SSL_session_reused(impl_->ssl) && impl_->resumed) {
    for (auto *certificate : impl_->resumed->chain) {
      if (X509_up_ref(certificate) != 1)
        return std::unexpected(openssl_error());
      state->chain.push_back(certificate);
    }
  } else {
    const auto count = chain ? sk_X509_num(chain) : 0;
    for (int i = 0; i < count; ++i) {
      auto *certificate = sk_X509_value(chain, i);
      if (X509_up_ref(certificate) != 1)
        return std::unexpected(openssl_error());
      state->chain.push_back(certificate);
    }
  }
  if (state->chain.empty())
    return std::unexpected(make_error_code(TlsError::session_unavailable));
  TlsSession session{state};
  if (!session.available())
    return std::unexpected(make_error_code(TlsError::session_unavailable));
  if (SSL_SESSION_up_ref(source) != 1)
    return std::unexpected(openssl_error());

  SSL_SESSION_free(impl_->captured_source);
  impl_->captured_source = source;
  impl_->revoke_session();
  impl_->captured = state;
  return session;
}

Result<TlsPeerIdentity> detail::TlsEngine::peer_identity() const
{
  std::lock_guard lock(impl_->mutex);
  const bool authenticated = impl_->credentials->server ? impl_->credentials->client_auth != TlsClientAuth::none
                                                        : impl_->credentials->verification != TlsVerification::none;
  auto *certificate = SSL_get0_peer_certificate(impl_->ssl);
  if (!impl_->verified || !authenticated || !certificate)
    return std::unexpected(make_error_code(TlsError::certificate_verification));

  return certificate_identity(certificate, impl_->credentials->limits.certificate_chain);
}

Result<std::vector<std::byte>> detail::TlsEngine::channel_binding() const
{
  std::lock_guard lock(impl_->mutex);
  auto *certificate = impl_->credentials->server ? SSL_get_certificate(impl_->ssl)
                                                 : SSL_get0_peer_certificate(impl_->ssl);
  if (!impl_->verified || !certificate)
    return std::unexpected(make_error_code(TlsError::certificate_verification));

  int digest = NID_undef;
  int key = NID_undef;
  int bits = 0;
  unsigned int flags = 0;
  if (X509_get_signature_info(certificate, &digest, &key, &bits, &flags) != 1)
    return std::unexpected(openssl_error());
  if (digest == NID_md5 || digest == NID_sha1)
    digest = NID_sha256;
  const auto *algorithm = EVP_get_digestbynid(digest);
  if (!algorithm)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  std::vector<std::byte> value(EVP_MAX_MD_SIZE);
  unsigned int size = 0;
  if (X509_digest(certificate, algorithm, reinterpret_cast<unsigned char *>(value.data()), &size) != 1)
    return std::unexpected(openssl_error());
  value.resize(size);
  return value;
}

Result<std::vector<std::byte>> detail::TlsEngine::export_keying_material(
  std::string_view label,
  std::size_t size,
  std::optional<std::span<const std::byte>> context) const
{
  const std::array reserved{"client finished", "server finished", "master secret", "key expansion"};
  const bool reserved_label = std::any_of(reserved.begin(), reserved.end(), [label](std::string_view prefix) {
    return label.starts_with(prefix);
  });
  if (label.empty() || label.size() > 249 || label.find('\0') != label.npos || reserved_label || size == 0 ||
    size > 65536 || (context && context->size() > 65535))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);
  if (!impl_->verified)
    return std::unexpected(make_error_code(TlsError::protocol));

  ERR_clear_error();
  std::vector<std::byte> value(size);
  const auto *bytes = context ? reinterpret_cast<const unsigned char *>(context->data()) : nullptr;
  if (SSL_export_keying_material(
        impl_->ssl,
        reinterpret_cast<unsigned char *>(value.data()),
        value.size(),
        label.data(),
        label.size(),
        bytes,
        context ? context->size() : 0,
        context.has_value()) != 1)
    return std::unexpected(openssl_error());
  return value;
}

} // namespace weave
