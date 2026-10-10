#pragma once

#include "credentials.hpp"

namespace weave::detail {

Result<void> tls_configure_key_log(TlsCredentials &credentials, const std::string &path);
Error tls_key_log_error(const TlsCredentials &credentials) noexcept;

} // namespace weave::detail
