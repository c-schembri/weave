#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace bench::stress {

struct Workload {
  std::size_t bytes = 1024;
  int work = 0;
  bool uneven = false;
};

template <class T>
bool number(const char *argument, T &value)
{
  std::string_view text{argument};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

inline bool valid(Workload config)
{
  return config.bytes >= 16 && config.bytes <= 65536 && config.work >= 0 && config.work <= 2'000'000;
}

inline std::uint64_t cpu_work(std::uint64_t value, int iterations)
{
  for (int i = 0; i < iterations; ++i) {
    value = value * 1664525 + 1013904223;
    value ^= value >> 17;
  }
  return value;
}

inline void transform(std::span<std::byte> frame, Workload config)
{
  std::uint64_t seed = 0;
  std::memcpy(&seed, frame.data(), sizeof(seed));
  const auto iterations = config.uneven && (seed - 1) % 8 != 0 ? 0 : config.work;
  const auto result = cpu_work(seed, iterations);
  std::memcpy(frame.data(), &result, sizeof(result));
}

} // namespace bench::stress
