#pragma once
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

namespace example {

inline std::optional<std::uint16_t> port(int argc, char **argv)
{
  if (argc == 1)
    return std::uint16_t{8080};
  if (argc == 2) {
    unsigned value = 0;
    const auto *end = argv[1] + std::strlen(argv[1]);
    const auto parsed = std::from_chars(argv[1], end, value);
    if (parsed.ec == std::errc{} && parsed.ptr == end && value <= 65535)
      return static_cast<std::uint16_t>(value);
  }
  std::fputs("Usage: echo_server [port: 0-65535]\n", stderr);
  return std::nullopt;
}

inline int fail(const char *operation, std::string_view message)
{
  std::fprintf(stderr, "%s: %.*s\n", operation, static_cast<int>(message.size()), message.data());
  return 1;
}

inline void listening(unsigned port)
{
  std::printf("Listening on 127.0.0.1:%u\n", port);
  std::fflush(stdout);
}

} // namespace example
