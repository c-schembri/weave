#pragma once

#include <weave/task.hpp>
#include <memory>
#include <string>
#include <vector>

namespace weave {

enum class TlsVersion {
  tls12,
  tls13
};

enum class TlsError {
  protocol = 1,
  certificate_verification,
  truncated,
  closed
};

Error make_error_code(TlsError error) noexcept;

struct TlsClientOptions {
  std::string ca_file;
  std::vector<std::string> alpn;
  TlsVersion min_version = TlsVersion::tls12;
  TlsVersion max_version = TlsVersion::tls13;
};

struct TlsServerOptions {
  std::string certificate_file;
  std::string private_key_file;
  std::string private_key_password;
  std::vector<std::string> alpn;
  TlsVersion min_version = TlsVersion::tls12;
  TlsVersion max_version = TlsVersion::tls13;
};

namespace detail {

struct TlsCredentials;
class TlsEngine;

} // namespace detail

class TlsContext {
  friend class detail::TlsEngine;
  std::shared_ptr<detail::TlsCredentials> credentials_;

  explicit TlsContext(std::shared_ptr<detail::TlsCredentials> credentials) noexcept
      : credentials_(std::move(credentials))
  {
  }

public:
  static Result<TlsContext> client(TlsClientOptions options = {});
  static Result<TlsContext> server(TlsServerOptions options);
};

} // namespace weave

namespace std {

template <>
struct is_error_code_enum<weave::TlsError> : true_type {};

} // namespace std
