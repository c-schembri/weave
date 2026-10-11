#pragma once

#include <string>
#include <chrono>
#include <optional>
#include <span>
#include <string_view>

namespace weave::pg {

/// Whether a connection keyword is parsed directly, requires explicit loading, or is unsupported/unknown.
enum class OptionSupport {
  parse,
  load,
  unsupported,
  unrecognized
};

/// Static keyword/default metadata. Views have static storage; secret marks sensitive option values.
struct OptionDescriptor {
  std::string_view keyword;
  std::string_view environment;
  std::optional<std::string_view> default_value;
  OptionSupport support = OptionSupport::unsupported;
  bool secret = false;
  std::string_view constraints;
};

/// Borrow static Weave option metadata for the program lifetime; no ambient configuration reads.
/// Defaults are not environment/service-resolved values or negotiated session settings.
std::span<const OptionDescriptor> option_schema() noexcept;

/// Owned explicit source-discovery policy for Options::load/defaults, not ordinary parse/connect.
/// libpq_compatibility opts into conventional discovery and weaker libpq TLS defaults.
struct ConfigSources {
  bool environment = true;
  bool user_files = true;
  std::string service;
  std::string service_file;
  std::string system_service_file;
  std::string password_file;
  bool ldap = false;
  std::chrono::milliseconds ldap_timeout{2000};
  bool system_files = true;
  // Explicit migration profile: libpq TLS defaults and conventional credential discovery.
  bool libpq_compatibility = false;
};

/// Owned source selection provenance; a selected path does not prove it was opened or supplied credentials.
struct ConfigurationOrigin {
  ConfigSources sources;
  std::optional<std::string> service;
  std::optional<std::string> service_file;
  // Selected path, not proof that the file was opened or supplied a credential.
  std::optional<std::string> password_file;
};

} // namespace weave::pg
