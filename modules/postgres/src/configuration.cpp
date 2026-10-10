#include <weave/postgres/connection.hpp>
#include "options.hpp"
#include "secret.hpp"
#include "ldap.hpp"
#include "configuration_paths.hpp"
#include <openssl/crypto.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace weave::pg {

namespace {

using Fields = detail::OptionFields;

std::error_code invalid()
{
  return std::make_error_code(std::errc::invalid_argument);
}

std::error_code unsupported()
{
  return std::make_error_code(std::errc::operation_not_supported);
}

std::error_code file_error()
{
  return std::error_code{errno ? errno : EIO, std::generic_category()};
}

struct TextCleanup {
  std::string &text;

  ~TextCleanup()
  {
    OPENSSL_cleanse(text.data(), text.size());
  }
};

bool valid_text(std::string_view text)
{
  if (text.size() > detail::maximum_connection_field || text.find('\0') != std::string_view::npos)
    return false;
#ifdef _WIN32
  if (!text.empty() &&
    !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0))
    return false;
#endif
  return true;
}

#ifdef _WIN32

Result<std::string> utf8(std::wstring_view value)
{
  if (value.empty())
    return std::string{};
  auto length = WideCharToMultiByte(
    CP_UTF8,
    WC_ERR_INVALID_CHARS,
    value.data(),
    static_cast<int>(value.size()),
    nullptr,
    0,
    nullptr,
    nullptr);
  if (!length || length > static_cast<int>(detail::maximum_connection_field))
    return std::unexpected(invalid());
  std::string output(static_cast<std::size_t>(length), '\0');
  if (!WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        output.data(),
        length,
        nullptr,
        nullptr))
    return std::unexpected(invalid());
  return output;
}

#endif

Result<std::optional<std::string>> environment(std::string_view name)
{
#ifdef _WIN32
  std::wstring key(name.begin(), name.end());
  SetLastError(ERROR_SUCCESS);
  auto size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
  if (!size) {
    auto error = GetLastError();
    if (error == ERROR_SUCCESS || error == ERROR_ENVVAR_NOT_FOUND)
      return std::optional<std::string>{};
    return std::unexpected(std::error_code{static_cast<int>(error), std::system_category()});
  }
  if (size > detail::maximum_connection_field + 1)
    return std::unexpected(invalid());
  std::wstring value(size, L'\0');
  SetLastError(ERROR_SUCCESS);
  auto length = GetEnvironmentVariableW(key.c_str(), value.data(), size);
  if (!length) {
    if (GetLastError() == ERROR_SUCCESS)
      return std::optional<std::string>{};
    return std::unexpected(invalid());
  }
  if (length >= size)
    return std::unexpected(invalid());
  value.resize(length);
  auto converted = utf8(value);
  SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
  if (!converted)
    return std::unexpected(converted.error());
  return std::optional<std::string>{std::move(*converted)};
#else
  std::string key{name};
  auto value = std::getenv(key.c_str());
  if (!value || !*value)
    return std::optional<std::string>{};
  auto length = strnlen(value, detail::maximum_connection_field + 1);
  if (length > detail::maximum_connection_field)
    return std::unexpected(invalid());
  return std::optional<std::string>{std::string{value, length}};
#endif
}

Result<std::pair<std::string, std::string>> identity()
{
#ifdef _WIN32
  std::array<wchar_t, 1024> name{};
  DWORD size = static_cast<DWORD>(name.size());
  if (!GetUserNameW(name.data(), &size))
    return std::unexpected(std::error_code{static_cast<int>(GetLastError()), std::system_category()});
  auto user = utf8(std::wstring_view{name.data(), size - 1});
  if (!user)
    return std::unexpected(user.error());
  return std::pair{std::move(*user), std::string{}};
#else
  std::vector<char> buffer(4096);
  for (;;) {
    passwd information{};
    passwd *found = nullptr;
    int error = getpwuid_r(geteuid(), &information, buffer.data(), buffer.size(), &found);
    if (error == ERANGE && buffer.size() < detail::maximum_connection_field) {
      buffer.resize(buffer.size() * 2);
      continue;
    }
    if (error)
      return std::unexpected(std::error_code{error, std::generic_category()});
    if (!found || !information.pw_name || !information.pw_dir)
      return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    return std::pair{std::string{information.pw_name}, std::string{information.pw_dir}};
  }
#endif
}

Result<std::optional<u64>> peer_user(std::string_view name)
{
  if (name.empty())
    return std::optional<u64>{};
#ifdef _WIN32
  return std::unexpected(unsupported());
#else
  if (!valid_text(name))
    return std::unexpected(invalid());
  std::string key{name};
  std::vector<char> buffer(4096);
  u64 user = 0;
  for (;;) {
    passwd information{};
    passwd *found = nullptr;
    int error = getpwnam_r(key.c_str(), &information, buffer.data(), buffer.size(), &found);
    if (error == ERANGE && buffer.size() < detail::maximum_connection_field) {
      buffer.resize(std::min(buffer.size() * 2, detail::maximum_connection_field));
      continue;
    }
    if (error)
      return std::unexpected(std::error_code{error, std::generic_category()});
    if (!found || !information.pw_name)
      return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    if (name != information.pw_name)
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
    user = information.pw_uid;
    break;
  }

  // Match libpq's canonical reverse-lookup policy at explicit configuration time.
  for (;;) {
    passwd information{};
    passwd *found = nullptr;
    int error = getpwuid_r(static_cast<uid_t>(user), &information, buffer.data(), buffer.size(), &found);
    if (error == ERANGE && buffer.size() < detail::maximum_connection_field) {
      buffer.resize(std::min(buffer.size() * 2, detail::maximum_connection_field));
      continue;
    }
    if (error)
      return std::unexpected(std::error_code{error, std::generic_category()});
    if (!found || !information.pw_name)
      return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
    if (name != information.pw_name)
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
    return std::optional<u64>{user};
  }
#endif
}

std::filesystem::path native_path(std::string_view value)
{
  std::u8string bytes(value.begin(), value.end());
  return std::filesystem::path{bytes};
}

std::string path_text(const std::filesystem::path &path)
{
  auto bytes = path.u8string();
  return std::string{bytes.begin(), bytes.end()};
}

struct FileCloser {
  void operator()(std::FILE *file) const noexcept
  {
    std::fclose(file);
  }
};

using File = std::unique_ptr<std::FILE, FileCloser>;

Result<File> open_file(std::string_view path, bool password)
{
  if (path.empty() || !valid_text(path))
    return std::unexpected(invalid());
  auto native = native_path(path);
#ifdef _WIN32
  auto handle = CreateFileW(
    native.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ,
    nullptr,
    OPEN_EXISTING,
    FILE_FLAG_SEQUENTIAL_SCAN,
    nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return std::unexpected(std::error_code{static_cast<int>(GetLastError()), std::system_category()});
  BY_HANDLE_FILE_INFORMATION information{};
  bool regular = GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &information) &&
    !(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  if (!regular) {
    CloseHandle(handle);
    return std::unexpected(invalid());
  }
  int descriptor = _open_osfhandle(reinterpret_cast<std::intptr_t>(handle), _O_RDONLY | _O_BINARY);
  if (descriptor == -1) {
    auto error = file_error();
    CloseHandle(handle);
    return std::unexpected(error);
  }
  auto file = _fdopen(descriptor, "rb");
  if (!file) {
    auto error = file_error();
    _close(descriptor);
    return std::unexpected(error);
  }
  static_cast<void>(password);
#else
  int descriptor = open(native.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (descriptor == -1)
    return std::unexpected(file_error());
  struct stat information{};
  if (fstat(descriptor, &information) != 0) {
    auto error = file_error();
    close(descriptor);
    return std::unexpected(error);
  }
  if (!S_ISREG(information.st_mode) || (password && (information.st_mode & 077))) {
    close(descriptor);
    auto error = S_ISREG(information.st_mode) ? std::errc::permission_denied : std::errc::invalid_argument;
    return std::unexpected(std::make_error_code(error));
  }
  auto file = fdopen(descriptor, "rb");
  if (!file) {
    auto error = file_error();
    close(descriptor);
    return std::unexpected(error);
  }
#endif
  return File{file};
}

Result<detail::SecretText> read_file(std::string_view path, bool password)
{
  auto file = open_file(path, password);
  if (!file)
    return std::unexpected(file.error());
  detail::SecretText text;
  std::array<char, 4096> buffer{};
  for (;;) {
    auto count = std::fread(buffer.data(), 1, buffer.size(), file->get());
    if (std::ferror(file->get())) {
      auto error = file_error();
      OPENSSL_cleanse(buffer.data(), buffer.size());
      OPENSSL_cleanse(text.data(), text.size());
      return std::unexpected(error);
    }
    if (count > detail::maximum_connection_input - text.size()) {
      OPENSSL_cleanse(buffer.data(), buffer.size());
      OPENSSL_cleanse(text.data(), text.size());
      return std::unexpected(std::make_error_code(std::errc::file_too_large));
    }
    text.insert(text.end(), buffer.data(), buffer.data() + count);
    OPENSSL_cleanse(buffer.data(), buffer.size());
    if (count != buffer.size())
      break;
  }
  if (std::ranges::find(text, '\0') != text.end()) {
    OPENSSL_cleanse(text.data(), text.size());
    return std::unexpected(invalid());
  }
  return text;
}

bool whitespace(char value)
{
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\v' || value == '\f';
}

std::string_view trim(std::string_view text)
{
  while (!text.empty() && whitespace(text.front()))
    text.remove_prefix(1);
  while (!text.empty() && whitespace(text.back()))
    text.remove_suffix(1);
  return text;
}

std::string_view next_line(std::string_view &text)
{
  auto end = text.find('\n');
  auto line = text.substr(0, end);
  text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
  if (line.ends_with('\r'))
    line.remove_suffix(1);
  return line;
}

Result<bool> service_file(
  Fields &fields,
  std::string_view path,
  std::string_view name,
  bool required,
  const ConfigSources &sources)
{
  auto contents = read_file(path, false);
  if (!contents) {
    if (!required && contents.error() == std::errc::no_such_file_or_directory)
      return false;
    return std::unexpected(contents.error());
  }
  std::string_view remaining{contents->data(), contents->size()};
  bool selected = false;
  std::size_t ldap_attempts = 0;
  while (!remaining.empty()) {
    auto line = trim(next_line(remaining));
    if (line.empty() || line.starts_with('#'))
      continue;
    if (line.starts_with('[')) {
      if (selected)
        return true;
      auto close = line.find(']');
      if (close == std::string_view::npos)
        return std::unexpected(invalid());
      selected = line.substr(1, close - 1) == name;
      continue;
    }
    if (!selected)
      continue;
    if (line.starts_with("ldap")) {
      if (!sources.ldap)
        return std::unexpected(unsupported());
      if (++ldap_attempts > 16)
        return std::unexpected(std::make_error_code(std::errc::value_too_large));
      auto query = detail::ldap_url(line);
      if (!query)
        return std::unexpected(query.error());
      auto values = detail::ldap_query(*query, sources.ldap_timeout);
      if (!values)
        return std::unexpected(values.error());
      if (!*values)
        continue;

      detail::SecretText options;
      for (const auto &value : **values) {
        if (options.size() == detail::maximum_connection_input ||
          value.size() > detail::maximum_connection_input - options.size() - 1)
          return std::unexpected(std::make_error_code(std::errc::value_too_large));
        options.insert(options.end(), value.begin(), value.end());
        options.push_back('\n');
      }
      auto loaded = detail::parse_service_options(fields, std::string_view{options.data(), options.size()});
      if (!loaded)
        return std::unexpected(loaded.error());
      return true;
    }
    auto equals = line.find('=');
    if (equals == std::string_view::npos)
      return std::unexpected(invalid());
    auto key = line.substr(0, equals);
    auto value = line.substr(equals + 1);
    if (!detail::known_option(key))
      return std::unexpected(invalid());
    if (key == "service")
      return std::unexpected(unsupported());
    if (!fields.get(key)) {
      if (auto result = fields.put(key, std::string{value}); !result)
        return std::unexpected(result.error());
    }
  }
  return selected;
}

Result<void> merge(Fields &destination, const Fields &source)
{
  for (const auto &[key, value] : source.values) {
    if (auto result = destination.put(key, value); !result)
      return result;
  }
  return {};
}

struct PasswordEntry {
  std::array<std::string, 4> fields;
  std::array<bool, 4> wildcards{};
  std::string_view password;
};

std::optional<PasswordEntry> password_entry(std::string_view line)
{
  if (line.empty() || line.starts_with('#'))
    return std::nullopt;
  PasswordEntry entry;
  std::size_t cursor = 0;
  for (std::size_t index = 0; index < entry.fields.size(); ++index) {
    auto start = cursor;
    bool terminated = false;
    while (cursor < line.size()) {
      char byte = line[cursor++];
      if (byte == ':') {
        terminated = true;
        break;
      }
      if (byte == '\\') {
        if (cursor == line.size())
          return std::nullopt;
        byte = line[cursor++];
      }
      if (entry.fields[index].size() == detail::maximum_connection_field)
        return std::nullopt;
      entry.fields[index].push_back(byte);
    }
    if (!terminated)
      return std::nullopt;
    entry.wildcards[index] = line.substr(start, cursor - start - 1) == "*";
  }
  entry.password = line.substr(cursor);
  return entry;
}

Result<std::string> password_value(std::string_view text)
{
  std::string password;
  for (std::size_t cursor = 0; cursor < text.size(); ++cursor) {
    auto byte = text[cursor];
    if (byte == '\\') {
      if (++cursor == text.size()) {
        OPENSSL_cleanse(password.data(), password.size());
        return std::unexpected(invalid());
      }
      byte = text[cursor];
    }
    if (password.size() == detail::maximum_connection_field) {
      OPENSSL_cleanse(password.data(), password.size());
      return std::unexpected(invalid());
    }
    password.push_back(byte);
  }
  return password;
}

bool matches(const PasswordEntry &entry, const Host &host, const Options &options)
{
  auto port = std::to_string(host.port);
  auto database = options.database.empty() ? std::string_view{options.user} : std::string_view{options.database};
  if (options.replication == Replication::physical)
    database = "replication";
  const std::array<std::string_view, 4> values{host.name, port, database, options.user};
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (!entry.wildcards[index] && entry.fields[index] != values[index])
      return false;
  }
  return true;
}

Result<void> passwords(Options &options, std::string_view path, bool required)
{
  if (!options.password.empty() || path.empty())
    return {};
  auto contents = read_file(path, true);
  if (!contents) {
    if (!required && contents.error() == std::errc::no_such_file_or_directory)
      return {};
    return std::unexpected(contents.error());
  }
  std::string_view remaining{contents->data(), contents->size()};
  Host single{options.host, options.port};
  while (!remaining.empty()) {
    auto entry = password_entry(next_line(remaining));
    if (!entry)
      continue;
    bool match = options.hosts.empty() && matches(*entry, single, options);
    for (const auto &host : options.hosts)
      match = match || (!host.password && matches(*entry, host, options));
    if (!match)
      continue;
    auto password = password_value(entry->password);
    if (!password)
      return std::unexpected(password.error());
    TextCleanup password_cleanup{*password};
    if (options.hosts.empty()) {
      options.password = *password;
      return {};
    }
    for (auto &host : options.hosts) {
      if (!host.password && matches(*entry, host, options))
        host.password = *password;
    }
  }
  return {};
}

Result<void> environment_fields(Fields &fields)
{
  for (const auto &option : option_schema()) {
    // Service-file discovery and the conditional legacy alias have dedicated paths below.
    bool separate = option.keyword == "servicefile" || option.keyword == "requiressl";
    if (option.environment.empty() || separate)
      continue;

    auto value = environment(option.environment);
    if (!value)
      return std::unexpected(value.error());
    if (*value) {
      if (auto result = fields.put(option.keyword, std::move(**value)); !result)
        return result;
    }
  }
  if (!fields.get("sslmode")) {
    auto legacy = environment("PGREQUIRESSL");
    if (!legacy)
      return std::unexpected(legacy.error());
    if (*legacy) {
      if (auto result = fields.put("requiressl", std::move(**legacy)); !result)
        return result;
    }
  }
  return {};
}

Result<std::string> configured_home()
{
#ifdef _WIN32
  auto appdata = environment("APPDATA");
  if (!appdata)
    return std::unexpected(appdata.error());
  if (*appdata)
    return path_text(native_path(**appdata) / "postgresql");
  return std::string{};
#else
  auto home = environment("HOME");
  if (!home)
    return std::unexpected(home.error());
  if (*home)
    return std::move(**home);
  auto user = identity();
  if (!user)
    return std::unexpected(user.error());
  return std::move(user->second);
#endif
}

Result<void> load_service(
  Fields &fields,
  const Fields &explicit_fields,
  const ConfigSources &sources,
  std::string_view home,
  ConfigurationOrigin &origin)
{
  std::string name = sources.service;
  if (name.empty()) {
    if (auto value = fields.get("service"))
      name = *value;
  }
  if (auto value = explicit_fields.get("service"))
    name = *value;
  if (name.empty())
    return {};
  if (!valid_text(name) || name.find_first_of("\r\n[]") != std::string::npos)
    return std::unexpected(invalid());

  std::string user_file = sources.service_file;
  std::string system_file = sources.system_service_file;
  bool required_user = !user_file.empty();
  bool required_system = !system_file.empty();
  if (sources.environment) {
    if (user_file.empty()) {
      auto value = environment("PGSERVICEFILE");
      if (!value)
        return std::unexpected(value.error());
      if (*value) {
        user_file = std::move(**value);
        required_user = true;
      }
    }
    if (system_file.empty()) {
      auto directory = environment("PGSYSCONFDIR");
      if (!directory)
        return std::unexpected(directory.error());
      if (*directory) {
        system_file = path_text(native_path(**directory) / "pg_service.conf");
        required_system = true;
      }
    }
  }
  if (user_file.empty() && !home.empty())
    user_file = path_text(native_path(home) / ".pg_service.conf");
  if (system_file.empty() && sources.system_files && !configured_system_service_directory.empty()) {
    if (!valid_text(configured_system_service_directory))
      return std::unexpected(invalid());
    system_file = path_text(native_path(configured_system_service_directory) / "pg_service.conf");
  }

  Fields service;
  bool found = false;
  if (!user_file.empty()) {
    auto selected = service_file(service, user_file, name, required_user, sources);
    if (!selected)
      return std::unexpected(selected.error());
    found = *selected;
    if (found)
      origin.service_file = user_file;
  }
  if (!found && !system_file.empty()) {
    auto selected = service_file(service, system_file, name, required_system, sources);
    if (!selected)
      return std::unexpected(selected.error());
    found = *selected;
    if (found)
      origin.service_file = system_file;
  }
  if (!found)
    return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));
  origin.service = name;
  return merge(fields, service);
}

Result<void> compatibility_fields(Fields &fields, const ConfigSources &sources, std::string_view home)
{
  if (!sources.libpq_compatibility)
    return {};

  auto mode = fields.get("sslmode");
  if (!mode || mode->empty()) {
    auto legacy = fields.get("requiressl");
    if (legacy && *legacy != "0" && *legacy != "1")
      return std::unexpected(invalid());
    auto root = fields.get("sslrootcert");
    const auto default_mode = root && *root == "system" ? "verify-full"
      : legacy && *legacy == "1"                        ? "require"
                                                        : "prefer";
    if (auto stored = fields.put("sslmode", default_mode); !stored)
      return stored;
  }
  fields.values.erase("requiressl");
  if (!fields.get("max_protocol_version")) {
    if (auto stored = fields.put("max_protocol_version", "3.0"); !stored)
      return stored;
  }
  const auto selected_mode = fields.get("sslmode");
  if ((selected_mode && *selected_mode == "disable") || home.empty())
    return {};

#ifdef _WIN32
  auto directory = native_path(home);
#else
  auto directory = native_path(home) / ".postgresql";
#endif
  const std::array files{
    std::pair{"sslrootcert", "root.crt"},
    std::pair{"sslcert", "postgresql.crt"},
    std::pair{"sslkey", "postgresql.key"},
    std::pair{"sslcrl", "root.crl"}};
  for (const auto &[key, filename] : files) {
    auto configured = fields.get(key);
    if (configured && !configured->empty())
      continue;
    if (key == std::string_view{"sslkey"} && !fields.get("sslcert"))
      continue;
    auto path = directory / filename;
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error)
      return std::unexpected(error);
    const auto policy = fields.get("sslmode");
    const bool required_root = key == std::string_view{"sslrootcert"} && policy &&
      (*policy == "verify-ca" || *policy == "verify-full");
    const bool required_key = key == std::string_view{"sslkey"} && fields.get("sslcert");
    if (!exists && !required_root && !required_key)
      continue;
    if (auto stored = fields.put(key, path_text(path)); !stored)
      return stored;
  }
  return {};
}

Result<void> compatibility_key([[maybe_unused]] const Options &options, [[maybe_unused]] const ConfigSources &sources)
{
#ifndef _WIN32
  if (sources.libpq_compatibility && options.tls_options && !options.tls_options->certificate_file.empty()) {
    const auto &path = options.tls_options->private_key_file;
    struct stat information{};
    if (stat(path.c_str(), &information) != 0)
      return std::unexpected(file_error());
    const bool private_owner = information.st_uid == geteuid() && !(information.st_mode & 0077);
    const bool root_group = information.st_uid == 0 && !(information.st_mode & 0037);
    if (!S_ISREG(information.st_mode) || (!private_owner && !root_group))
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
  }
#endif
  return {};
}

} // namespace

Result<Options> Options::load(std::string_view connection_string, ConfigSources sources)
{
  if (sources.ldap &&
    (sources.ldap_timeout <= std::chrono::milliseconds::zero() || sources.ldap_timeout > std::chrono::minutes{1}))
    return std::unexpected(invalid());
  const std::array source_fields{
    std::string_view{sources.service},
    std::string_view{sources.service_file},
    std::string_view{sources.system_service_file},
    std::string_view{sources.password_file}};
  for (auto field : source_fields) {
    if (!valid_text(field))
      return std::unexpected(invalid());
  }

  Fields explicit_fields;
  if (auto parsed = detail::parse_option_fields(explicit_fields, connection_string); !parsed)
    return std::unexpected(parsed.error());
  for (const auto &[key, value] : explicit_fields.values) {
    if (!detail::known_option(key))
      return std::unexpected(invalid());
  }

  Fields fields;
  if (sources.environment) {
    if (auto loaded = environment_fields(fields); !loaded)
      return std::unexpected(loaded.error());
  }
  std::string home;
  if (sources.user_files) {
    auto directory = configured_home();
    if (!directory)
      return std::unexpected(directory.error());
    home = std::move(*directory);
  }
  ConfigurationOrigin origin;
  origin.sources = sources;
  if (auto loaded = load_service(fields, explicit_fields, sources, home, origin); !loaded)
    return std::unexpected(loaded.error());
  if (auto merged = merge(fields, explicit_fields); !merged)
    return std::unexpected(merged.error());
  if (auto compatible = compatibility_fields(fields, sources, home); !compatible)
    return std::unexpected(compatible.error());
  if (fields.get("sslmode"))
    fields.values.erase("requiressl");

  std::string password_file = sources.password_file;
  bool required_password = !password_file.empty();
  auto specified_password = fields.get("passfile");
  if (specified_password && (password_file.empty() || explicit_fields.get("passfile"))) {
    password_file = *specified_password;
    required_password = !password_file.empty();
  }
  if (password_file.empty() && !home.empty()) {
#ifdef _WIN32
    password_file = path_text(native_path(home) / "pgpass.conf");
#else
    password_file = path_text(native_path(home) / ".pgpass");
#endif
  }
  if (!password_file.empty())
    origin.password_file = password_file;
  fields.values.erase("service");
  fields.values.erase("passfile");

  std::string required_peer;
  if (auto name = fields.get("requirepeer"))
    required_peer = *name;
  fields.values.erase("requirepeer");

  auto user = fields.get("user");
  if (!user || user->empty()) {
    auto account = identity();
    if (!account)
      return std::unexpected(account.error());
    if (auto stored = fields.put("user", std::move(account->first)); !stored)
      return std::unexpected(stored.error());
  }
  auto options = detail::options_from_fields(fields);
  if (!options)
    return std::unexpected(options.error());
  detail::OptionsCleanup cleanup{*options};
  if (auto key = compatibility_key(*options, sources); !key)
    return std::unexpected(key.error());

  auto peer = peer_user(required_peer);
  if (!peer)
    return std::unexpected(peer.error());
  options->required_peer_user = *peer;

  if (sources.environment) {
    const std::array settings{
      std::pair{"PGDATESTYLE", "DateStyle"},
      std::pair{"PGTZ", "TimeZone"},
      std::pair{"PGGEQO", "geqo"}};
    for (const auto &[variable, setting] : settings) {
      auto value = environment(variable);
      if (!value)
        return std::unexpected(value.error());
      if (*value)
        options->settings.emplace_back(setting, std::move(**value));
    }
  }
  if (auto loaded = passwords(*options, password_file, required_password); !loaded)
    return std::unexpected(loaded.error());
  options->origin = std::move(origin);
  cleanup.dismiss();
  return options;
}

} // namespace weave::pg
