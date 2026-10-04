#pragma once
#include <benchmark/benchmark.h>
#include <cstdint>

namespace bench {

__declspec(noinline) inline std::uint64_t integer_work(std::uint64_t value)
{
  for (int i = 0; i < 100000; ++i) {
    value = value * 1664525 + 1013904223;
    value ^= value >> 17;
  }
  benchmark::DoNotOptimize(value);
  return value;
}

inline std::uint64_t skew_work(std::uint64_t seed, std::size_t workers)
{
  const int repetitions = (seed - 1) % workers == 0 ? 16 : 1;
  for (int i = 0; i < repetitions; ++i)
    seed = integer_work(seed);
  return seed;
}

} // namespace bench
