#pragma once
#include <windows.h>
#include <algorithm>
#include <bit>
#include <span>
#include <vector>

namespace experiment {

struct CpuPartition {
  DWORD_PTR client = 0, peer = 0;
};

inline CpuPartition partition_cpus(std::span<const DWORD_PTR> physical_cores, DWORD_PTR allowed)
{
  CpuPartition result;
  std::size_t selected = 0;
  DWORD_PTR seen = 0;
  for (const auto core : physical_cores) {
    if (!core || (seen & core))
      return {};
    seen |= core;
    const auto available = core & allowed;
    if (!available)
      continue;
    const auto logical = DWORD_PTR{1} << std::countr_zero(available);
    // Interleave one peer core with two client cores; never share an SMT core.
    if (selected % 3 == 0)
      result.peer |= logical;
    else
      result.client |= logical;
    if (++selected == 12)
      return result;
  }
  return {};
}

inline CpuPartition partition_ci_cpus(std::span<const DWORD_PTR> physical_cores, DWORD_PTR allowed)
{
  std::vector<DWORD_PTR> selected;
  DWORD_PTR seen = 0;
  for (const auto core : physical_cores) {
    if (!core || (seen & core))
      return {};
    seen |= core;
    if (const auto available = core & allowed)
      selected.push_back(DWORD_PTR{1} << std::countr_zero(available));
  }
  if (selected.size() < 2)
    return {};

  const auto workers = std::min<std::size_t>(2, selected.size() / 2);
  CpuPartition result;
  for (std::size_t i = 0; i < workers; ++i) {
    result.peer |= selected[i * 2];
    result.client |= selected[i * 2 + 1];
  }
  return result;
}

inline CpuPartition isolated_cpus(bool ci = false)
{
  if (GetActiveProcessorGroupCount() != 1)
    return {};
  DWORD_PTR allowed = 0, system = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &allowed, &system))
    return {};
  DWORD bytes = 0;
  if (GetLogicalProcessorInformation(nullptr, &bytes) || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    return {};
  std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
  if (!GetLogicalProcessorInformation(info.data(), &bytes))
    return {};
  std::vector<DWORD_PTR> cores;
  for (const auto &entry : info) {
    if (entry.Relationship == RelationProcessorCore)
      cores.push_back(entry.ProcessorMask);
  }
  std::sort(cores.begin(), cores.end());
  return ci ? partition_ci_cpus(cores, allowed) : partition_cpus(cores, allowed);
}

} // namespace experiment
