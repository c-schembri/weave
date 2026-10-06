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

TEST_CASE("CI benchmarks adapt to runner cores without sharing SMT siblings")
{
  const std::array<DWORD_PTR, 4> cores{3, 12, 48, 192};
  const auto dual = experiment::partition_ci_cpus(cores, 255);
  CHECK(std::popcount(dual.peer) == 2);
  CHECK(std::popcount(dual.client) == 2);
  for (const auto core : cores) {
    CHECK(std::popcount(core & (dual.client | dual.peer)) == 1);
    const bool shared = (core & dual.client) && (core & dual.peer);
    CHECK_FALSE(shared);
  }

  const auto single = experiment::partition_ci_cpus(std::span(cores).first(2), 255);
  CHECK(std::popcount(single.peer) == 1);
  CHECK(std::popcount(single.client) == 1);
  const auto restricted = experiment::partition_ci_cpus(cores, 0xAA);
  CHECK(std::popcount(restricted.peer) == 2);
  CHECK(std::popcount(restricted.client) == 2);
  CHECK(((restricted.client | restricted.peer) & ~DWORD_PTR{0xAA}) == 0);
  CHECK(experiment::partition_ci_cpus(std::span(cores).first(1), 255).peer == 0);
  CHECK(experiment::partition_ci_cpus(cores, 3).client == 0);

  const std::array<DWORD_PTR, 2> overlapping{3, 6};
  const std::array<DWORD_PTR, 2> invalid{0, 12};
  CHECK(experiment::partition_ci_cpus(overlapping, 255).peer == 0);
  CHECK(experiment::partition_ci_cpus(invalid, 255).client == 0);
}

TEST_CASE("runtime scaling fixes client cores and never oversubscribes physical cores")
{
  std::array<DWORD_PTR, 12> cores;
  for (std::size_t i = 0; i < cores.size(); ++i)
    cores[i] = DWORD_PTR{3} << (i * 2);
  const auto client_mask = experiment::partition_scaling_cpus(cores, 0xFFFFFF, 1, 4).client;
  const std::array<std::size_t, 4> supported{1, 2, 4, 8};
  for (const auto count : supported) {
    const auto masks = experiment::partition_scaling_cpus(cores, 0xFFFFFF, count, 4);
    CHECK(std::popcount(masks.peer) == count);
    CHECK(std::popcount(masks.client) == 4);
    CHECK(masks.client == client_mask);
    for (const auto core : cores) {
      CHECK(std::popcount(core & (masks.peer | masks.client)) <= 1);
      const bool shared = (core & masks.peer) && (core & masks.client);
      CHECK_FALSE(shared);
    }
  }
  CHECK(experiment::partition_scaling_cpus(cores, 0xFFFFFF, 16, 4).peer == 0);
  CHECK(experiment::partition_scaling_cpus(cores, 0xFFFFFF, 32, 4).client == 0);
  CHECK(experiment::partition_scaling_cpus(cores, 0xFFFFFF, 0, 4).peer == 0);
  CHECK(experiment::partition_scaling_cpus(cores, 0xFFFFFF, 4, 0).peer == 0);
  const auto restricted = experiment::partition_scaling_cpus(cores, 0xAAAAAA, 8, 4);
  CHECK(std::popcount(restricted.peer) == 8);
  CHECK(((restricted.peer | restricted.client) & ~DWORD_PTR{0xAAAAAA}) == 0);
  cores[1] = cores[0];
  CHECK(experiment::partition_scaling_cpus(cores, 0xFFFFFF, 4, 4).peer == 0);
}

TEST_CASE("runtime scaling can select thirty-two real server cores when available")
{
  std::array<DWORD_PTR, 40> cores;
  for (std::size_t i = 0; i < cores.size(); ++i)
    cores[i] = DWORD_PTR{1} << i;
  const auto allowed = (DWORD_PTR{1} << cores.size()) - 1;
  const std::array<std::size_t, 6> counts{1, 2, 4, 8, 16, 32};
  DWORD_PTR client = 0;
  for (const auto count : counts) {
    const auto masks = experiment::partition_scaling_cpus(cores, allowed, count, 4);
    CHECK(std::popcount(masks.peer) == count);
    CHECK(std::popcount(masks.client) == 4);
    CHECK((masks.peer & masks.client) == 0);
    if (client)
      CHECK(masks.client == client);
    client = masks.client;
  }
}
