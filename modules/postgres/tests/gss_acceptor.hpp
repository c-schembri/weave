#pragma once

#include "gss.hpp"
#include <gssapi/gssapi.h>
#include <openssl/crypto.h>
#include <algorithm>

namespace weave::pg::test {

struct NativeBytes {
  gss_buffer_desc value{0, nullptr};

  ~NativeBytes()
  {
    if (value.value)
      OPENSSL_cleanse(value.value, value.length);
    OM_uint32 minor = 0;
    gss_release_buffer(&minor, &value);
  }

  detail::SecretStorage<std::byte> copy() const
  {
    if (!value.length)
      return {};
    auto *first = static_cast<const std::byte *>(value.value);
    return {first, first + value.length};
  }
};

struct GssAcceptor {
  gss_cred_id_t credentials = GSS_C_NO_CREDENTIAL;
  gss_cred_id_t delegated = GSS_C_NO_CREDENTIAL;
  gss_ctx_id_t context = GSS_C_NO_CONTEXT;
  bool complete = false;

  ~GssAcceptor()
  {
    OM_uint32 minor = 0;
    if (context != GSS_C_NO_CONTEXT)
      gss_delete_sec_context(&minor, &context, GSS_C_NO_BUFFER);
    if (delegated != GSS_C_NO_CREDENTIAL)
      gss_release_cred(&minor, &delegated);
    if (credentials != GSS_C_NO_CREDENTIAL)
      gss_release_cred(&minor, &credentials);
  }

  bool start()
  {
    OM_uint32 minor = 0;
    return gss_acquire_cred(&minor, GSS_C_NO_NAME, 0, GSS_C_NO_OID_SET, GSS_C_ACCEPT, &credentials, nullptr, nullptr) ==
      GSS_S_COMPLETE;
  }

  Result<detail::SecretStorage<std::byte>> accept(std::span<std::byte> bytes)
  {
    gss_buffer_desc input{bytes.size(), bytes.data()};
    NativeBytes output;
    OM_uint32 minor = 0;
    OM_uint32 flags = 0;
    auto major = gss_accept_sec_context(
      &minor,
      &context,
      credentials,
      &input,
      GSS_C_NO_CHANNEL_BINDINGS,
      nullptr,
      nullptr,
      &output.value,
      &flags,
      nullptr,
      &delegated);
    if ((major != GSS_S_COMPLETE && major != GSS_S_CONTINUE_NEEDED) || output.value.length > 65536)
      return std::unexpected(make_error_code(Error::authentication));
    complete = major == GSS_S_COMPLETE;
    return output.copy();
  }

  Result<detail::SecretStorage<std::byte>> wrap(std::span<const std::byte> bytes, bool encrypted = true)
  {
    gss_buffer_desc input{bytes.size(), const_cast<std::byte *>(bytes.data())};
    NativeBytes output;
    OM_uint32 minor = 0;
    int confidential = 0;
    auto status = gss_wrap(&minor, context, encrypted, GSS_C_QOP_DEFAULT, &input, &confidential, &output.value);
    if (status != GSS_S_COMPLETE || confidential != encrypted || output.value.length > 65536)
      return std::unexpected(make_error_code(Error::authentication));
    return output.copy();
  }

  bool unwrap(std::span<std::byte> record, std::span<const std::byte> expected)
  {
    gss_buffer_desc input{record.size(), record.data()};
    NativeBytes output;
    OM_uint32 minor = 0;
    int confidential = 0;
    auto status = gss_unwrap(&minor, context, &input, &output.value, &confidential, nullptr);
    if (status != GSS_S_COMPLETE || !confidential || output.value.length > 65536)
      return false;
    auto plaintext = output.copy();
    return std::ranges::equal(plaintext, expected);
  }
};

} // namespace weave::pg::test
