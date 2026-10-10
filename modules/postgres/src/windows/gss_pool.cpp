#include "../gss_pool.hpp"
#include <windows.h>
#include <process.h>
#include <vector>
#include <cerrno>

namespace weave::pg::detail {

namespace {

std::error_code win_error() noexcept
{
  return {static_cast<int>(GetLastError()), std::system_category()};
}

struct Token {
  HANDLE value = nullptr;

  ~Token()
  {
    if (value)
      CloseHandle(value);
  }
};

} // namespace

struct GssPool::Platform {
  Token identity;
  std::vector<HANDLE> threads;

  static unsigned __stdcall enter(void *state) noexcept
  {
    auto &pool = *static_cast<GssPool *>(state);
    const bool impersonated = ImpersonateLoggedOnUser(pool.platform_->identity.value) != FALSE;
    auto error = impersonated ? std::error_code{} : win_error();
    pool.worker(error);
    if (impersonated)
      weave::detail::require(RevertToSelf() != FALSE);
    return 0;
  }
};

GssPool::GssPool(GssContextOptions options) : platform_(std::make_unique<Platform>()), options_(std::move(options))
{
}

GssPool::~GssPool()
{
  stop();
  for (auto thread : platform_->threads) {
    weave::detail::require(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
  }
  drained();
}

Result<void> GssPool::start()
{
  if (!options_.credential_cache.empty())
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
  Token effective;
  SECURITY_IMPERSONATION_LEVEL level = SecurityImpersonation;
  if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &effective.value)) {
    auto error = GetLastError();
    if (error != ERROR_NO_TOKEN)
      return std::unexpected(std::error_code(static_cast<int>(error), std::system_category()));
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &effective.value))
      return std::unexpected(win_error());
  } else {
    DWORD size = 0;
    if (!GetTokenInformation(effective.value, TokenImpersonationLevel, &level, sizeof(level), &size))
      return std::unexpected(win_error());
    if (level < SecurityImpersonation)
      return std::unexpected(std::make_error_code(std::errc::permission_denied));
  }
  if (!DuplicateTokenEx(
        effective.value,
        TOKEN_QUERY | TOKEN_IMPERSONATE,
        nullptr,
        level,
        TokenImpersonation,
        &platform_->identity.value))
    return std::unexpected(win_error());

  platform_->threads.reserve(options_.workers);
  for (std::size_t index = 0; index < options_.workers; ++index) {
    auto thread = _beginthreadex(nullptr, 0, Platform::enter, this, 0, nullptr);
    if (!thread)
      return std::unexpected(std::error_code(errno ? errno : EAGAIN, std::generic_category()));
    platform_->threads.push_back(reinterpret_cast<HANDLE>(thread));
  }
  return wait_started();
}

} // namespace weave::pg::detail
