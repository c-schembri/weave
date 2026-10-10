#include "key_log.hpp"
#include <array>
#include <mutex>
#include <cstring>
#include <cerrno>
#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace weave {

namespace {

Error denied() noexcept
{
  return std::make_error_code(std::errc::permission_denied);
}

#ifdef _WIN32

Error native_error() noexcept
{
  return {static_cast<int>(GetLastError()), std::system_category()};
}

struct LocalMemory {
  void *value = nullptr;

  ~LocalMemory()
  {
    LocalFree(value);
  }
};

Result<std::vector<std::byte>> user_token()
{
  HANDLE token = nullptr;
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
    if (GetLastError() != ERROR_NO_TOKEN || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
      return std::unexpected(native_error());
  }
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<std::byte> data(size);
  const bool read = size && GetTokenInformation(token, TokenUser, data.data(), size, &size);
  const auto error = read ? Error{} : native_error();
  CloseHandle(token);
  if (!read)
    return std::unexpected(error);
  return data;
}

Result<void> private_file(HANDLE file, PSID user)
{
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(file, &information))
    return std::unexpected(native_error());
  if (GetFileType(file) != FILE_TYPE_DISK ||
    (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
    return std::unexpected(denied());

  LocalMemory descriptor;
  PSID owner = nullptr;
  PACL acl = nullptr;
  const auto status = GetSecurityInfo(
    file,
    SE_FILE_OBJECT,
    OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
    &owner,
    nullptr,
    &acl,
    nullptr,
    reinterpret_cast<PSECURITY_DESCRIPTOR *>(&descriptor.value));
  if (status != ERROR_SUCCESS)
    return std::unexpected(Error{static_cast<int>(status), std::system_category()});
  if (!owner || !EqualSid(owner, user) || !acl)
    return std::unexpected(denied());

  std::array<std::byte, SECURITY_MAX_SID_SIZE> system{};
  DWORD size = static_cast<DWORD>(system.size());
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &size))
    return std::unexpected(native_error());
  for (DWORD index = 0; index < acl->AceCount; ++index) {
    void *entry = nullptr;
    if (!GetAce(acl, index, &entry))
      return std::unexpected(native_error());
    auto *header = static_cast<ACE_HEADER *>(entry);
    if (header->AceFlags & INHERIT_ONLY_ACE)
      continue;
    if (header->AceType == ACCESS_DENIED_ACE_TYPE)
      continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE)
      return std::unexpected(denied());
    auto *allowed = static_cast<ACCESS_ALLOWED_ACE *>(entry);
    auto *sid = &allowed->SidStart;
    if (allowed->Mask && !EqualSid(sid, user) && !EqualSid(sid, system.data()))
      return std::unexpected(denied());
  }
  return {};
}

#endif

} // namespace

class detail::TlsKeyLog {
  std::mutex mutex_;
  Error error_;
#ifdef _WIN32
  HANDLE file_ = INVALID_HANDLE_VALUE;
#else
  int file_ = -1;
#endif

public:
  ~TlsKeyLog()
  {
#ifdef _WIN32
    if (file_ != INVALID_HANDLE_VALUE)
      CloseHandle(file_);
#else
    if (file_ >= 0)
      ::close(file_);
#endif
  }

  Result<void> open(const std::string &path)
  {
    if (path.size() > 65536 || path.find('\0') != std::string::npos)
      return std::unexpected(std::make_error_code(std::errc::invalid_argument));
#ifdef _WIN32
    const auto length = MultiByteToWideChar(
      CP_UTF8,
      MB_ERR_INVALID_CHARS,
      path.data(),
      static_cast<int>(path.size()),
      nullptr,
      0);
    if (!length)
      return std::unexpected(native_error());
    std::wstring wide(length, L'\0');
    if (!MultiByteToWideChar(
          CP_UTF8,
          MB_ERR_INVALID_CHARS,
          path.data(),
          static_cast<int>(path.size()),
          wide.data(),
          length))
      return std::unexpected(native_error());
    auto token = user_token();
    if (!token)
      return std::unexpected(token.error());
    auto *user = reinterpret_cast<TOKEN_USER *>(token->data())->User.Sid;
    LocalMemory sid;
    if (!ConvertSidToStringSidW(user, reinterpret_cast<LPWSTR *>(&sid.value)))
      return std::unexpected(native_error());
    const auto *user_sid = static_cast<wchar_t *>(sid.value);
    // Elevated tokens can default to an Administrators-group owner.
    std::wstring policy = L"O:";
    policy += user_sid;
    policy += L"D:P(A;;FA;;;SY)(A;;FA;;;";
    policy += user_sid;
    policy += L")";
    LocalMemory descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          policy.c_str(),
          SDDL_REVISION_1,
          reinterpret_cast<PSECURITY_DESCRIPTOR *>(&descriptor.value),
          nullptr))
      return std::unexpected(native_error());
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor.value, FALSE};
    file_ = CreateFileW(
      wide.c_str(),
      FILE_APPEND_DATA | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &attributes,
      OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
    if (file_ == INVALID_HANDLE_VALUE)
      return std::unexpected(native_error());
    return private_file(file_, user);
#else
    file_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (file_ < 0)
      return std::unexpected(Error{errno, std::generic_category()});
    struct stat information{};
    if (fstat(file_, &information) != 0)
      return std::unexpected(Error{errno, std::generic_category()});
    if (!S_ISREG(information.st_mode) || information.st_uid != geteuid() || (information.st_mode & 0077))
      return std::unexpected(denied());
    return {};
#endif
  }

  void append(const char *line) noexcept
  {
    std::lock_guard lock(mutex_);
    if (error_)
      return;
    std::array<char, 1024> text{};
    const auto length = strnlen(line, text.size() - 1);
    if (!length || length == text.size() - 1) {
      error_ = std::make_error_code(std::errc::value_too_large);
      return;
    }
    std::memcpy(text.data(), line, length);
    text[length] = '\n';
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(file_, text.data(), static_cast<DWORD>(length + 1), &written, nullptr))
      error_ = native_error();
    else if (written != length + 1)
      error_ = std::make_error_code(std::errc::io_error);
    SecureZeroMemory(text.data(), text.size());
#else
    ssize_t written;
    do {
      written = ::write(file_, text.data(), length + 1);
    } while (written < 0 && errno == EINTR);
    if (written < 0)
      error_ = {errno, std::generic_category()};
    else if (static_cast<std::size_t>(written) != length + 1)
      error_ = std::make_error_code(std::errc::io_error);
    OPENSSL_cleanse(text.data(), text.size());
#endif
  }

  Error error() noexcept
  {
    std::lock_guard lock(mutex_);
    return error_;
  }
};

Result<void> detail::tls_configure_key_log(TlsCredentials &credentials, const std::string &path)
{
  if (path.empty())
    return {};
  auto log = std::make_shared<TlsKeyLog>();
  if (auto opened = log->open(path); !opened)
    return opened;
  credentials.key_log = std::move(log);
  SSL_CTX_set_app_data(credentials.context, &credentials);
  SSL_CTX_set_keylog_callback(
    credentials.context,
    +[](const SSL *ssl, const char *line) {
      auto *owner = static_cast<TlsCredentials *>(SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
      owner->key_log->append(line);
    });
  return {};
}

Error detail::tls_key_log_error(const TlsCredentials &credentials) noexcept
{
  return credentials.key_log ? credentials.key_log->error() : Error{};
}

} // namespace weave
