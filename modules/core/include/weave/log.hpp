#pragma once

#include <cstdio>
#include <system_error>

namespace weave {

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

#define WEAVE_LOG_INFO(format, ...) WEAVE_DETAIL_LOG("INFO", format __VA_OPT__(, ) __VA_ARGS__)
#define WEAVE_LOG_WARN(format, ...) WEAVE_DETAIL_LOG("WARN", format __VA_OPT__(, ) __VA_ARGS__)
#define WEAVE_LOG_ERROR(format, ...) WEAVE_DETAIL_LOG("ERROR", format __VA_OPT__(, ) __VA_ARGS__)

#ifndef NDEBUG
#define WEAVE_LOG_DEBUG(format, ...) WEAVE_DETAIL_LOG("DEBUG", format __VA_OPT__(, ) __VA_ARGS__)
#else
#define WEAVE_LOG_DEBUG(...) \
  do {                       \
  } while (false)
#endif
