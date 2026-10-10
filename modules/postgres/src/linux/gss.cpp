#include "../gss.hpp"
#include <openssl/crypto.h>
#include <bit>
#if defined(WEAVE_POSTGRES_GSSAPI)
#include <gssapi/gssapi.h>
#include <gssapi/gssapi_ext.h>
#include <gssapi/gssapi_krb5.h>
#endif

namespace weave::pg::detail {

namespace {

#if defined(WEAVE_POSTGRES_GSSAPI)

std::string status_text(OM_uint32 value, int kind)
{
  std::string result;
  OM_uint32 context = 0;
  for (unsigned part = 0; part < 16; ++part) {
    OM_uint32 minor = 0;
    gss_buffer_desc text{0, nullptr};
    auto major = gss_display_status(&minor, value, kind, GSS_C_NO_OID, &context, &text);
    const std::size_t separator = result.empty() ? 0 : 2;
    if (major == GSS_S_COMPLETE && text.value && text.length <= 4096 - result.size() - separator) {
      if (!result.empty())
        result += "; ";
      result.append(static_cast<const char *>(text.value), text.length);
    }
    gss_release_buffer(&minor, &text);
    if (major != GSS_S_COMPLETE || context == 0 || result.size() >= 4094)
      break;
  }
  return result;
}

class GssCategory final : public std::error_category {
public:
  const char *name() const noexcept override
  {
    return "weave.postgres.gssapi";
  }

  std::string message(int value) const override
  {
    auto result = status_text(std::bit_cast<OM_uint32>(value), GSS_C_GSS_CODE);
    return result.empty() ? "GSSAPI authentication failed" : result;
  }
};

const GssCategory errors;

struct Output {
  gss_buffer_desc token{0, nullptr};

  ~Output()
  {
    if (token.value) {
      OPENSSL_cleanse(token.value, token.length);
      OM_uint32 minor = 0;
      gss_release_buffer(&minor, &token);
    }
  }
};

#endif

} // namespace

struct Gss::State {
  std::string diagnostic;
#if defined(WEAVE_POSTGRES_GSSAPI)
  gss_ctx_id_t context = GSS_C_NO_CONTEXT;
  gss_name_t target = GSS_C_NO_NAME;
  gss_cred_id_t credentials = GSS_C_NO_CREDENTIAL;
  OM_uint32 requested = GSS_C_MUTUAL_FLAG;
  bool require_mutual = true;
  bool protect = false;
  std::size_t plaintext_limit = 0;
  bool finished = false;
  bool failed = true;
  unsigned steps = 0;

  std::error_code native_error(OM_uint32 major, OM_uint32 minor)
  {
    // A mapped minor can describe native zero even when the major rejects a token.
    // Preserve the failure code, and capture thread-local provider details here.
    diagnostic = status_text(major, GSS_C_GSS_CODE);
    if (diagnostic.empty())
      diagnostic = "GSSAPI authentication failed";
    if (minor != 0) {
      auto text = status_text(minor, GSS_C_MECH_CODE);
      diagnostic += "; mechanism status " + std::to_string(minor);
      if (!text.empty())
        diagnostic += ": " + text;
    }
    return {std::bit_cast<int>(major), errors};
  }

  ~State()
  {
    OM_uint32 minor = 0;
    if (context != GSS_C_NO_CONTEXT)
      gss_delete_sec_context(&minor, &context, GSS_C_NO_BUFFER);
    if (target != GSS_C_NO_NAME)
      gss_release_name(&minor, &target);
    if (credentials != GSS_C_NO_CREDENTIAL)
      gss_release_cred(&minor, &credentials);
  }

  Result<GssToken> advance(std::span<const std::byte> input)
  {
    if (failed || finished || steps >= 64 || input.size() > 65536)
      return std::unexpected(make_error_code(Error::authentication));
    failed = true;
    ++steps;

    gss_buffer_desc received{input.size(), const_cast<std::byte *>(input.data())};
    Output output;
    OM_uint32 minor = 0;
    OM_uint32 flags = 0;
    auto major = gss_init_sec_context(
      &minor,
      credentials,
      &context,
      target,
      gss_mech_krb5,
      requested,
      0,
      GSS_C_NO_CHANNEL_BINDINGS,
      input.empty() ? GSS_C_NO_BUFFER : &received,
      nullptr,
      &output.token,
      &flags,
      nullptr);
    if (major != GSS_S_COMPLETE && major != GSS_S_CONTINUE_NEEDED)
      return std::unexpected(native_error(major, minor));
    if (output.token.length > 65536 || (output.token.length != 0 && !output.token.value))
      return std::unexpected(make_error_code(Error::resource_limit));

    GssToken token;
    token.mechanism = GssToken::Mechanism::kerberos;
    token.complete = major == GSS_S_COMPLETE;
    token.mutual = (flags & GSS_C_MUTUAL_FLAG) != 0;
    token.delegated = (flags & GSS_C_DELEG_FLAG) != 0;
    if (token.complete && require_mutual && !token.mutual) {
      diagnostic = "The negotiated mechanism did not authenticate the server mutually";
      return std::unexpected(make_error_code(Error::authentication));
    }
    if (token.complete && protect) {
      const auto required = GSS_C_MUTUAL_FLAG | GSS_C_REPLAY_FLAG | GSS_C_SEQUENCE_FLAG | GSS_C_CONF_FLAG |
        GSS_C_INTEG_FLAG;
      if ((flags & required) != required) {
        diagnostic = "The negotiated context lacks required GSS transport protections";
        return std::unexpected(make_error_code(Error::authentication));
      }

      OM_uint32 maximum = 0;
      major = gss_wrap_size_limit(
        &minor,
        context,
        1,
        GSS_C_QOP_DEFAULT,
        static_cast<OM_uint32>(gss_record_limit),
        &maximum);
      if (major != GSS_S_COMPLETE)
        return std::unexpected(native_error(major, minor));
      if (maximum == 0 || maximum > gss_record_limit)
        return std::unexpected(make_error_code(Error::resource_limit));
      plaintext_limit = maximum;
    }
    if (output.token.length != 0) {
      auto *bytes = static_cast<const std::byte *>(output.token.value);
      token.bytes.assign(bytes, bytes + output.token.length);
    }
    finished = token.complete;
    failed = false;
    return token;
  }
#endif
};

Gss::Gss() = default;
Gss::~Gss() = default;

bool Gss::available() noexcept
{
#if defined(WEAVE_POSTGRES_GSSAPI)
  return true;
#else
  return false;
#endif
}

Result<GssToken> Gss::start(std::string_view host, Authentication method, const GssOptions &options)
{
  if (state_ || !valid_gss_target(host, options.service) ||
    (method != Authentication::gss && method != Authentication::sspi) || options.credential_cache.size() > 65536 ||
    options.credential_cache.find('\0') != std::string::npos)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
#if defined(WEAVE_POSTGRES_GSSAPI)
  state_ = std::make_unique<State>();
  state_->require_mutual = options.require_mutual;
  state_->protect = options.protect;
  if (options.protect) {
    state_->requested |= GSS_C_REPLAY_FLAG | GSS_C_SEQUENCE_FLAG | GSS_C_CONF_FLAG | GSS_C_INTEG_FLAG;
    state_->require_mutual = true;
  }
  if (options.delegate)
    state_->requested |= GSS_C_DELEG_FLAG;

  auto principal = options.service + "@" + std::string(host);
  gss_buffer_desc name{principal.size(), principal.data()};
  OM_uint32 minor = 0;
  auto major = gss_import_name(&minor, &name, GSS_C_NT_HOSTBASED_SERVICE, &state_->target);
  if (major != GSS_S_COMPLETE)
    return std::unexpected(state_->native_error(major, minor));

  if (!options.credential_cache.empty()) {
    gss_key_value_element_desc entry{"ccache", options.credential_cache.c_str()};
    gss_key_value_set_desc store{1, &entry};
    major = gss_acquire_cred_from(
      &minor,
      GSS_C_NO_NAME,
      0,
      GSS_C_NO_OID_SET,
      GSS_C_INITIATE,
      &store,
      &state_->credentials,
      nullptr,
      nullptr);
    if (major != GSS_S_COMPLETE)
      return std::unexpected(state_->native_error(major, minor));
  }
  state_->failed = false;
  return state_->advance({});
#else
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
}

Result<GssToken> Gss::next(std::span<const std::byte> input)
{
  if (!state_)
    return std::unexpected(make_error_code(Error::authentication));
#if defined(WEAVE_POSTGRES_GSSAPI)
  if (input.empty() || input.size() > 65536) {
    state_->failed = true;
    return std::unexpected(make_error_code(Error::authentication));
  }
  return state_->advance(input);
#else
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
}

bool Gss::complete() const noexcept
{
#if defined(WEAVE_POSTGRES_GSSAPI)
  return state_ && state_->finished && !state_->failed;
#else
  return false;
#endif
}

std::size_t Gss::plaintext_limit() const noexcept
{
#if defined(WEAVE_POSTGRES_GSSAPI)
  return state_ ? state_->plaintext_limit : 0;
#else
  return 0;
#endif
}

Result<SecretStorage<std::byte>> Gss::wrap(std::span<const std::byte> input)
{
#if defined(WEAVE_POSTGRES_GSSAPI)
  if (!complete() || !state_->protect)
    return std::unexpected(make_error_code(Error::authentication));
  if (input.size() > state_->plaintext_limit)
    return std::unexpected(make_error_code(Error::resource_limit));

  state_->failed = true;
  gss_buffer_desc plaintext{input.size(), const_cast<std::byte *>(input.data())};
  Output output;
  OM_uint32 minor = 0;
  int confidential = 0;
  auto major = gss_wrap(&minor, state_->context, 1, GSS_C_QOP_DEFAULT, &plaintext, &confidential, &output.token);
  if (major != GSS_S_COMPLETE)
    return std::unexpected(state_->native_error(major, minor));
  if (!confidential) {
    state_->diagnostic = "The GSS provider produced an unencrypted transport record";
    return std::unexpected(make_error_code(Error::authentication));
  }
  if (output.token.length == 0 || output.token.length > gss_record_limit || !output.token.value)
    return std::unexpected(make_error_code(Error::resource_limit));

  auto *bytes = static_cast<const std::byte *>(output.token.value);
  SecretStorage<std::byte> record(bytes, bytes + output.token.length);
  state_->failed = false;
  return record;
#else
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
}

Result<SecretStorage<std::byte>> Gss::unwrap(std::span<const std::byte> input)
{
#if defined(WEAVE_POSTGRES_GSSAPI)
  if (!complete() || !state_->protect)
    return std::unexpected(make_error_code(Error::authentication));
  state_->failed = true;
  if (input.empty() || input.size() > gss_record_limit)
    return std::unexpected(make_error_code(Error::resource_limit));

  gss_buffer_desc record{input.size(), const_cast<std::byte *>(input.data())};
  Output output;
  OM_uint32 minor = 0;
  int confidential = 0;
  gss_qop_t quality = GSS_C_QOP_DEFAULT;
  auto major = gss_unwrap(&minor, state_->context, &record, &output.token, &confidential, &quality);
  // Supplementary replay, old-token, sequence and gap statuses are failures too.
  if (major != GSS_S_COMPLETE)
    return std::unexpected(state_->native_error(major, minor));
  if (!confidential || quality != GSS_C_QOP_DEFAULT) {
    state_->diagnostic = "The GSS transport record lacks required confidentiality or quality of protection";
    return std::unexpected(make_error_code(Error::authentication));
  }
  if (output.token.length > gss_record_limit || (output.token.length != 0 && !output.token.value))
    return std::unexpected(make_error_code(Error::resource_limit));

  SecretStorage<std::byte> plaintext;
  if (output.token.length != 0) {
    auto *bytes = static_cast<const std::byte *>(output.token.value);
    plaintext.assign(bytes, bytes + output.token.length);
  }
  state_->failed = false;
  return plaintext;
#else
  return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
}

std::string_view Gss::diagnostic() const noexcept
{
  return state_ ? std::string_view{state_->diagnostic} : std::string_view{};
}

} // namespace weave::pg::detail
