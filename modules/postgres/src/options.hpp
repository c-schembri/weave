#pragma once

#include <weave/postgres/connection.hpp>
#include <map>
#include "secret.hpp"

namespace weave::pg::detail {

inline constexpr std::size_t maximum_connection_input = 1024 * 1024;
inline constexpr std::size_t maximum_connection_field = 65536;

struct OptionFields {
  std::map<std::string, std::string, std::less<>> values;

  ~OptionFields();
  Result<void> put(std::string_view key, std::string value);
  const std::string *get(std::string_view key) const;
};

void clear_passwords(Options &options) noexcept;
void clear_tls_credentials(TlsClientOptions &options) noexcept;

struct OptionsCleanup {
  Options &options;
  char *password_data;
  std::size_t password_size;
  char *private_key_data;
  std::size_t private_key_size;
  char *private_key_locator_data;
  std::size_t private_key_locator_size;
  bool active = true;

  explicit OptionsCleanup(Options &options) noexcept;
  OptionsCleanup(const OptionsCleanup &) = delete;

  void dismiss() noexcept
  {
    active = false;
  }

  ~OptionsCleanup();
};

// A coroutine parameter owns this before initial suspension, unlike a body-local guard.
struct OwnedOptions {
  Options value;

  explicit OwnedOptions(Options &&options) noexcept;
  OwnedOptions(OwnedOptions &&other) noexcept;
  OwnedOptions(const OwnedOptions &) = delete;
  ~OwnedOptions();
  Options take() && noexcept;

private:
  OwnedOptions(
    Options &&options,
    char *password_data,
    std::size_t password_size,
    char *private_key_data,
    std::size_t private_key_size,
    char *private_key_locator_data,
    std::size_t private_key_locator_size) noexcept;
};

Result<void> parse_option_fields(OptionFields &fields, std::string_view input);
Result<void> parse_service_options(OptionFields &fields, std::string_view text);
Result<SecretText> decode_option_bytes(std::string_view text);
Result<Options> options_from_fields(const OptionFields &fields);
bool known_option(std::string_view key) noexcept;
Result<void> valid_authentication_policy(const AuthenticationPolicy &policy) noexcept;
bool local_host(std::string_view host) noexcept;
Result<std::string> local_socket_path(std::string_view host, u16 port);

} // namespace weave::pg::detail
