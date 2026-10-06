#include <weave/tls/detail/engine.hpp>
#include <weave/address.hpp>
#include <weave/io/detail/execution.hpp>
#include "trust.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <algorithm>
#include <array>
#include <mutex>

namespace weave {

namespace {

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

} // namespace

struct detail::TlsCredentials {
  SSL_CTX *context = nullptr;
  bool server = false;
  std::vector<unsigned char> alpn;
  std::string password;

  ~TlsCredentials()
  {
    if (context)
      SSL_CTX_free(context);
  }
};

namespace {

Result<std::shared_ptr<detail::TlsCredentials>> credentials(
  bool server,
  TlsVersion minimum,
  TlsVersion maximum,
  const std::vector<std::string> &protocols)
{
  if (!protocol(minimum) || !protocol(maximum) || protocol(minimum) > protocol(maximum))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto alpn = encode_alpn(protocols);
  if (!alpn)
    return std::unexpected(alpn.error());

  auto value = std::make_shared<detail::TlsCredentials>();
  value->server = server;
  value->alpn = std::move(*alpn);

  ERR_clear_error();
  value->context = SSL_CTX_new(TLS_method());
  if (!value->context)
    return std::unexpected(openssl_error());

  if (!SSL_CTX_set_min_proto_version(value->context, protocol(minimum)) ||
    !SSL_CTX_set_max_proto_version(value->context, protocol(maximum)))
    return std::unexpected(openssl_error());

  SSL_CTX_set_options(value->context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);

  // Loading an encrypted key must never prompt on stdin.
  SSL_CTX_set_default_passwd_cb(value->context, [](char *buffer, int size, int, void *state) {
    const auto &password = static_cast<detail::TlsCredentials *>(state)->password;
    if (size <= 0 || password.size() >= static_cast<std::size_t>(size))
      return 0;

    std::copy(password.begin(), password.end(), buffer);

    return static_cast<int>(password.size());
  });
  SSL_CTX_set_default_passwd_cb_userdata(value->context, value.get());

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

} // namespace

Error make_error_code(TlsError error) noexcept
{
  static TlsCategory category;

  return {static_cast<int>(error), category};
}

Result<TlsContext> TlsContext::client(TlsClientOptions options)
{
  if (options.ca_file.find('\0') != std::string::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto value = credentials(false, options.min_version, options.max_version, options.alpn);
  if (!value)
    return std::unexpected(value.error());

  auto *native = (*value)->context;
  SSL_CTX_set_verify(native, SSL_VERIFY_PEER, nullptr);
  ERR_clear_error();

  if (!options.ca_file.empty()) {
    if (!SSL_CTX_load_verify_locations(native, options.ca_file.c_str(), nullptr))
      return std::unexpected(openssl_error());
  } else {
    const bool defaults = SSL_CTX_set_default_verify_paths(native) == 1;
    const bool system = detail::system_roots(SSL_CTX_get_cert_store(native));
    if (!defaults && !system)
      return std::unexpected(openssl_error());
  }

  return TlsContext{std::move(*value)};
}

Result<TlsContext> TlsContext::server(TlsServerOptions options)
{
  if (options.certificate_file.empty() || options.private_key_file.empty() ||
    options.certificate_file.find('\0') != std::string::npos ||
    options.private_key_file.find('\0') != std::string::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto value = credentials(true, options.min_version, options.max_version, options.alpn);
  if (!value)
    return std::unexpected(value.error());

  (*value)->password = std::move(options.private_key_password);

  auto *native = (*value)->context;
  ERR_clear_error();
  if (!SSL_CTX_use_certificate_chain_file(native, options.certificate_file.c_str()) ||
    !SSL_CTX_use_PrivateKey_file(native, options.private_key_file.c_str(), SSL_FILETYPE_PEM) ||
    !SSL_CTX_check_private_key(native))
    return std::unexpected(openssl_error());

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
  u64 generation = 0;

  bool &flag(TlsOperation operation) noexcept
  {
    if (operation == TlsOperation::read)
      return reading;
    if (operation == TlsOperation::write)
      return writing;

    return exclusive;
  }

  ~Impl()
  {
    if (ssl)
      SSL_free(ssl);
  }

  TlsStep step(int result, std::size_t transferred = 0) noexcept
  {
    if (result == 1)
      return {TlsAction::ready, transferred, {}, generation};

    auto code = SSL_get_error(ssl, result);
    if (code == SSL_ERROR_WANT_READ && !input_eof)
      return {TlsAction::input, 0, {}, generation};
    if (code == SSL_ERROR_WANT_WRITE)
      return {TlsAction::output, 0, {}, generation};
    if (code == SSL_ERROR_ZERO_RETURN)
      return {TlsAction::eof, 0, {}, generation};

    if (SSL_get_verify_result(ssl) != X509_V_OK)
      error = make_error_code(TlsError::certificate_verification);
    else
      error = openssl_error();

    if (code == SSL_ERROR_WANT_READ && input_eof)
      error = make_error_code(TlsError::truncated);

    return {TlsAction::failed, 0, error, generation};
  }
};

Result<detail::TlsEngine> detail::TlsEngine::create(const TlsContext &context, bool server, const std::string &name)
{
  if (!context.credentials_ || context.credentials_->server != server ||
    (!server && (name.empty() || name.find('\0') != std::string::npos)))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));

  auto impl = std::make_unique<Impl>();
  impl->credentials = context.credentials_;

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
    SSL_set_hostflags(impl->ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

    auto ip = IpAddress::parse(name);
    if (ip) {
      if (!X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(impl->ssl), name.c_str()))
        return std::unexpected(openssl_error());
    } else if (!SSL_set1_host(impl->ssl, name.c_str()) || !SSL_set_tlsext_host_name(impl->ssl, name.c_str()))
      return std::unexpected(openssl_error());

    const auto &alpn = impl->credentials->alpn;
    if (!alpn.empty() && SSL_set_alpn_protos(impl->ssl, alpn.data(), static_cast<unsigned int>(alpn.size())) != 0)
      return std::unexpected(openssl_error());
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

Result<std::size_t> detail::TlsEngine::output(std::span<std::byte> buffer) noexcept
{
  std::lock_guard lock(impl_->mutex);
  if (impl_->error)
    return std::unexpected(impl_->error);

  auto *bio = SSL_get_wbio(impl_->ssl);
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
    constexpr std::size_t limit = 1024 * 1024;
    if (buffer.size() > limit || BIO_ctrl_pending(bio) > limit - buffer.size())
      return std::unexpected(std::make_error_code(std::errc::message_size));

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

} // namespace weave
