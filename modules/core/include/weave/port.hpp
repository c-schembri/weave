#pragma once

#include <charconv>
#include <cstdint>
#include <expected>
#include <string_view>
#include <system_error>

namespace weave {

// Strict decimal port in [0, 65535]; zero permits an OS-assigned local port.
[[nodiscard]] inline std::expected<std::uint16_t, std::error_code> parse_port(std::string_view text) noexcept
{
  if (!text.empty()) {
    std::uint16_t port = 0;
    const auto *end = text.data() + text.size();
    auto parsed = std::from_chars(text.data(), end, port);
    if (parsed.ec == std::errc{} && parsed.ptr == end)
      return port;
  }
  return std::unexpected(std::make_error_code(std::errc::invalid_argument));
}

} // namespace weave
