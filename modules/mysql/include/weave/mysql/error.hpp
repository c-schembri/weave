#pragma once

#include <weave/task.hpp>
#include <weave/types.hpp>

namespace weave::mysql {

/// Client/protocol failures; native transport and TLS errors retain their original categories.
enum class ClientError {
  invalid_options = 1,
  protocol,
  resource_limit,
  unsupported_authentication,
  tls_required,
  closed,
  busy,
  null_value,
  invalid_value,
  out_of_range,
  missing_column,
  ambiguous_column,
  multiple_results,
  unsupported
};

std::error_code make_error_code(ClientError error) noexcept;
/// MySQL server error numbers use a distinct category, not the client/protocol category.
std::error_code server_error(u16 number) noexcept;

} // namespace weave::mysql

template <>
struct std::is_error_code_enum<weave::mysql::ClientError> : std::true_type {};
