#pragma once

#include <cstdio>
#include <system_error>

namespace weave {

/// Print an error message and newline to the borrowed FILE; return exit status 1.
/// Synchronous, nonthrowing reporting only; this neither owns/closes stream nor propagates a Task failure.
[[nodiscard]] inline int report_error(const std::error_code &error, std::FILE *stream = stderr) noexcept
{
  (void)std::fprintf(stream, "%s\n", error.message().c_str());
  return 1;
}

} // namespace weave

#ifndef WEAVE_LOG_STREAM
#define WEAVE_LOG_STREAM stderr
#endif

#define WEAVE_DETAIL_LOG(level, format, ...)                                                     \
  do {                                                                                           \
    (void)std::fprintf(WEAVE_LOG_STREAM, "[" level "] " format "\n" __VA_OPT__(, ) __VA_ARGS__); \
  } while (false)

/// Synchronous printf-style logging to WEAVE_LOG_STREAM (stderr by default); format arguments are not type-checked.
#define WEAVE_LOG_INFO(format, ...) WEAVE_DETAIL_LOG("INFO", format __VA_OPT__(, ) __VA_ARGS__)
/// Log a warning with printf-style formatting; inputs are borrowed only during the call.
#define WEAVE_LOG_WARN(format, ...) WEAVE_DETAIL_LOG("WARN", format __VA_OPT__(, ) __VA_ARGS__)
/// Log an error with printf-style formatting; does not change control flow or return an error.
#define WEAVE_LOG_ERROR(format, ...) WEAVE_DETAIL_LOG("ERROR", format __VA_OPT__(, ) __VA_ARGS__)

#ifndef NDEBUG
/// Debug-only printf-style logging. In NDEBUG builds arguments are not evaluated.
#define WEAVE_LOG_DEBUG(format, ...) WEAVE_DETAIL_LOG("DEBUG", format __VA_OPT__(, ) __VA_ARGS__)
#else
/// Disabled debug logging: arguments are not evaluated in NDEBUG builds.
#define WEAVE_LOG_DEBUG(...) \
  do {                       \
  } while (false)
#endif
