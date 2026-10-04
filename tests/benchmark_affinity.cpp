#include <doctest/doctest.h>
#include "benchmark_affinity.hpp"
#include <array>

TEST_CASE("benchmark partitions use distinct physical cores, not SMT siblings")
{
  std::array<DWORD_PTR, 12> cores;
  for (std::size_t i = 0; i < cores.size(); ++i)
    cores[i] = DWORD_PTR{3} << (i * 2);
  const auto partition = experiment::partition_cpus(cores, 0xFFFFFF);
  CHECK(std::popcount(partition.client) == 8);
  CHECK(std::popcount(partition.peer) == 4);
  CHECK((partition.client & partition.peer) == 0);
  for (const auto core : cores) {
    CHECK(std::popcount(core & (partition.client | partition.peer)) == 1);
    CHECK_FALSE(((core & partition.client) && (core & partition.peer)));
  }
  const auto odd = experiment::partition_cpus(cores, 0xAAAAAA);
  CHECK(std::popcount(odd.client) == 8);
  CHECK(std::popcount(odd.peer) == 4);
  CHECK(((odd.client | odd.peer) & ~DWORD_PTR{0xAAAAAA}) == 0);
}

TEST_CASE("benchmark rejects insufficient or overlapping physical core masks")
{
  std::array<DWORD_PTR, 12> cores;
  for (std::size_t i = 0; i < cores.size(); ++i)
    cores[i] = DWORD_PTR{1} << i;
  CHECK(experiment::partition_cpus(std::span(cores).first(11), 0xFFF).client == 0);
  CHECK(experiment::partition_cpus(cores, 0x7FF).peer == 0);
  cores[1] = cores[0];
  CHECK(experiment::partition_cpus(cores, 0xFFF).client == 0);
  cores[1] = 0;
  CHECK(experiment::partition_cpus(cores, 0xFFF).peer == 0);
}
