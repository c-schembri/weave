#define SECURITY_WIN32
#include "../gss.hpp"
#include <windows.h>
#include <security.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <array>

namespace weave::pg::detail {

namespace {

std::error_code native_error(SECURITY_STATUS value) noexcept
{
  return {static_cast<int>(value), std::system_category()};
}

Result<std::wstring> wide(std::string_view text)
{
  auto
    size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (!size)
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  if (!MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        output.data(),
        size))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  return output;
}

struct Output {
  SecBuffer token{0, SECBUFFER_TOKEN, nullptr};
  SecBufferDesc descriptor{SECBUFFER_VERSION, 1, &token};

  ~Output()
  {
    if (token.pvBuffer) {
      OPENSSL_cleanse(token.pvBuffer, token.cbBuffer);
      FreeContextBuffer(token.pvBuffer);
    }
  }
};

bool contained(std::span<const std::byte> storage, const SecBuffer &buffer) noexcept
{
  const auto base = reinterpret_cast<std::uintptr_t>(storage.data());
  const auto position = reinterpret_cast<std::uintptr_t>(buffer.pvBuffer);
  return position >= base && position - base <= storage.size() && buffer.cbBuffer <= storage.size() - (position - base);
}

} // namespace

struct Gss::State {
  CredHandle credentials{};
  CtxtHandle context{};
  bool acquired = false;
  bool initialized = false;
  bool finished = false;
  bool failed = true;
  bool require_mutual = true;
  bool negotiate = false;
  bool protect = false;
  std::size_t plaintext_limit = 0;
  SecPkgContext_Sizes sizes{};
  unsigned steps = 0;
  ULONG requested = ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_MUTUAL_AUTH;
  std::wstring target;
  std::string diagnostic;

  std::error_code error(SECURITY_STATUS status)
  {
    auto result = native_error(status);
    diagnostic = result.message();
    return result;
  }

  ~State()
  {
    if (initialized)
      DeleteSecurityContext(&context);
    if (acquired)
      FreeCredentialsHandle(&credentials);
  }

  Result<GssToken> advance(std::span<const std::byte> input)
  {
    if (failed || finished || steps >= 64 || input.size() > 65536)
      return std::unexpected(make_error_code(Error::authentication));
    failed = true;
    ++steps;

    SecBuffer token{static_cast<ULONG>(input.size()), SECBUFFER_TOKEN, const_cast<std::byte *>(input.data())};
    SecBufferDesc received{SECBUFFER_VERSION, 1, &token};
    Output output;
    CtxtHandle next{};
    SecInvalidateHandle(&next);
    ULONG flags = 0;
    TimeStamp expiry{};
    auto status = InitializeSecurityContextW(
      &credentials,
      initialized ? &context : nullptr,
      target.data(),
      requested,
      0,
      SECURITY_NETWORK_DREP,
      initialized ? &received : nullptr,
      0,
      &next,
      &output.descriptor,
      &flags,
      &expiry);
    if (SecIsValidHandle(&next)) {
      context = next;
      initialized = true;
    }
    if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED && status != SEC_I_COMPLETE_NEEDED &&
      status != SEC_I_COMPLETE_AND_CONTINUE)
      return std::unexpected(error(status));

    if (!initialized)
      return std::unexpected(make_error_code(Error::authentication));
    if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
      auto completed = CompleteAuthToken(&context, &output.descriptor);
      if (completed != SEC_E_OK)
        return std::unexpected(error(completed));
    }
    if (output.token.cbBuffer > 65536 || (output.token.cbBuffer != 0 && !output.token.pvBuffer))
      return std::unexpected(make_error_code(Error::resource_limit));

    GssToken result;
    result.complete = status == SEC_E_OK || status == SEC_I_COMPLETE_NEEDED;
    if (result.complete && !negotiate)
      result.mechanism = GssToken::Mechanism::kerberos;
    if (result.complete && negotiate) {
      SecPkgContext_NegotiationInfoW negotiated{};
      auto queried = QueryContextAttributesW(&context, SECPKG_ATTR_NEGOTIATION_INFO, &negotiated);
      if (queried != SEC_E_OK)
        return std::unexpected(error(queried));
      if (negotiated.PackageInfo) {
        auto *name = negotiated.PackageInfo->Name;
        if (name && CompareStringOrdinal(name, -1, L"Kerberos", -1, TRUE) == CSTR_EQUAL)
          result.mechanism = GssToken::Mechanism::kerberos;
        else if (name && CompareStringOrdinal(name, -1, L"NTLM", -1, TRUE) == CSTR_EQUAL)
          result.mechanism = GssToken::Mechanism::ntlm;
        FreeContextBuffer(negotiated.PackageInfo);
      }
    }
    if (result.complete && result.mechanism == GssToken::Mechanism::unknown) {
      diagnostic = "The negotiated SSPI mechanism is not supported";
      return std::unexpected(make_error_code(Error::unsupported_authentication));
    }
    // NTLM can report this flag for optimized local logon; it is not a Kerberos server proof.
    result.mutual = result.mechanism == GssToken::Mechanism::kerberos && (flags & ISC_RET_MUTUAL_AUTH) != 0;
    result.delegated = result.mechanism == GssToken::Mechanism::kerberos && (flags & ISC_RET_DELEGATE) != 0;
    if (result.complete && require_mutual && !result.mutual) {
      diagnostic = "The negotiated mechanism did not authenticate the server mutually";
      return std::unexpected(make_error_code(Error::authentication));
    }
    if (result.complete && protect) {
      const auto required = ISC_RET_MUTUAL_AUTH | ISC_RET_REPLAY_DETECT | ISC_RET_SEQUENCE_DETECT |
        ISC_RET_CONFIDENTIALITY | ISC_RET_INTEGRITY;
      if (result.mechanism != GssToken::Mechanism::kerberos || (flags & required) != required) {
        diagnostic = "The negotiated context lacks required Kerberos transport protections";
        return std::unexpected(make_error_code(Error::authentication));
      }

      auto queried = QueryContextAttributesW(&context, SECPKG_ATTR_SIZES, &sizes);
      if (queried != SEC_E_OK)
        return std::unexpected(error(queried));
      if (sizes.cbSecurityTrailer >= gss_record_limit || sizes.cbBlockSize >= gss_record_limit ||
        sizes.cbSecurityTrailer + sizes.cbBlockSize >= gss_record_limit)
        return std::unexpected(make_error_code(Error::resource_limit));
      plaintext_limit = gss_record_limit - sizes.cbSecurityTrailer - sizes.cbBlockSize;
    }
    if (output.token.cbBuffer != 0) {
      auto *bytes = static_cast<const std::byte *>(output.token.pvBuffer);
      result.bytes.assign(bytes, bytes + output.token.cbBuffer);
    }
    finished = result.complete;
    failed = false;
    return result;
  }
};

Gss::Gss() = default;
Gss::~Gss() = default;

bool Gss::available() noexcept
{
  return true;
}

Result<GssToken> Gss::start(std::string_view host, Authentication method, const GssOptions &options)
{
  if (state_ || !valid_gss_target(host, options.service) ||
    (method != Authentication::gss && method != Authentication::sspi))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  if (!options.credential_cache.empty())
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));

  auto principal = wide(options.service + "/" + std::string(host));
  if (!principal)
    return std::unexpected(principal.error());

  state_ = std::make_unique<State>();
  state_->target = std::move(*principal);
  state_->require_mutual = options.require_mutual;
  state_->negotiate = method == Authentication::sspi;
  state_->protect = options.protect;
  if (!options.require_mutual)
    state_->requested &= ~ISC_REQ_MUTUAL_AUTH;
  if (options.delegate)
    state_->requested |= ISC_REQ_DELEGATE;
  if (options.protect) {
    state_->requested |= ISC_REQ_MUTUAL_AUTH | ISC_REQ_REPLAY_DETECT | ISC_REQ_SEQUENCE_DETECT |
      ISC_REQ_CONFIDENTIALITY | ISC_REQ_INTEGRITY;
    state_->require_mutual = true;
  }

  TimeStamp expiry{};
  auto package = method == Authentication::gss ? L"Kerberos" : L"Negotiate";
  auto status = AcquireCredentialsHandleW(
    nullptr,
    const_cast<wchar_t *>(package),
    SECPKG_CRED_OUTBOUND,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &state_->credentials,
    &expiry);
  if (status != SEC_E_OK)
    return std::unexpected(state_->error(status));
  state_->acquired = true;
  state_->failed = false;
  return state_->advance({});
}

Result<GssToken> Gss::next(std::span<const std::byte> input)
{
  if (!state_)
    return std::unexpected(make_error_code(Error::authentication));
  if (input.empty() || input.size() > 65536) {
    state_->failed = true;
    return std::unexpected(make_error_code(Error::authentication));
  }
  return state_->advance(input);
}

bool Gss::complete() const noexcept
{
  return state_ && state_->finished && !state_->failed;
}

std::size_t Gss::plaintext_limit() const noexcept
{
  return state_ ? state_->plaintext_limit : 0;
}

Result<SecretStorage<std::byte>> Gss::wrap(std::span<const std::byte> input)
{
  if (!complete() || !state_->protect)
    return std::unexpected(make_error_code(Error::authentication));
  if (input.size() > state_->plaintext_limit)
    return std::unexpected(make_error_code(Error::resource_limit));

  state_->failed = true;
  const auto trailer = state_->sizes.cbSecurityTrailer;
  const auto padding = state_->sizes.cbBlockSize;
  SecretStorage<std::byte> storage(trailer + input.size() + padding);
  std::copy(input.begin(), input.end(), storage.begin() + trailer);
  std::array buffers{
    SecBuffer{trailer, SECBUFFER_TOKEN, storage.data()},
    SecBuffer{static_cast<ULONG>(input.size()), SECBUFFER_DATA, storage.data() + trailer},
    SecBuffer{padding, SECBUFFER_PADDING, storage.data() + trailer + input.size()}};
  SecBufferDesc descriptor{SECBUFFER_VERSION, static_cast<ULONG>(buffers.size()), buffers.data()};
  auto status = EncryptMessage(&state_->context, 0, &descriptor, 0);
  if (status != SEC_E_OK)
    return std::unexpected(state_->error(status));

  const std::array<ULONG, 3> types{SECBUFFER_TOKEN, SECBUFFER_DATA, SECBUFFER_PADDING};
  SecretStorage<std::byte> record;
  record.reserve(storage.size());
  for (std::size_t index = 0; index < buffers.size(); ++index) {
    const auto &buffer = buffers[index];
    if (buffer.BufferType != types[index] || !contained(storage, buffer) ||
      buffer.cbBuffer > gss_record_limit - record.size())
      return std::unexpected(make_error_code(Error::resource_limit));
    auto *bytes = static_cast<const std::byte *>(buffer.pvBuffer);
    record.insert(record.end(), bytes, bytes + buffer.cbBuffer);
  }
  if (record.empty())
    return std::unexpected(make_error_code(Error::resource_limit));
  state_->failed = false;
  return record;
}

Result<SecretStorage<std::byte>> Gss::unwrap(std::span<const std::byte> input)
{
  if (!complete() || !state_->protect)
    return std::unexpected(make_error_code(Error::authentication));
  state_->failed = true;
  if (input.empty() || input.size() > gss_record_limit)
    return std::unexpected(make_error_code(Error::resource_limit));

  SecretStorage<std::byte> storage(input.begin(), input.end());
  std::array buffers{
    SecBuffer{static_cast<ULONG>(storage.size()), SECBUFFER_STREAM, storage.data()},
    SecBuffer{0, SECBUFFER_DATA, nullptr}};
  SecBufferDesc descriptor{SECBUFFER_VERSION, static_cast<ULONG>(buffers.size()), buffers.data()};
  ULONG quality = 0;
  auto status = DecryptMessage(&state_->context, &descriptor, 0, &quality);
  if (status != SEC_E_OK)
    return std::unexpected(state_->error(status));
  if (quality != 0) {
    state_->diagnostic = "The SSPI transport record lacks required confidentiality or quality of protection";
    return std::unexpected(make_error_code(Error::authentication));
  }
  const auto &data = buffers[1];
  if (data.BufferType != SECBUFFER_DATA || data.cbBuffer > gss_record_limit ||
    (data.cbBuffer != 0 && !contained(storage, data)))
    return std::unexpected(make_error_code(Error::resource_limit));

  SecretStorage<std::byte> plaintext;
  if (data.cbBuffer != 0) {
    auto *bytes = static_cast<const std::byte *>(data.pvBuffer);
    plaintext.assign(bytes, bytes + data.cbBuffer);
  }
  state_->failed = false;
  return plaintext;
}

std::string_view Gss::diagnostic() const noexcept
{
  return state_ ? std::string_view{state_->diagnostic} : std::string_view{};
}

} // namespace weave::pg::detail
