#pragma once

#include "address.hpp"
#include <weave/resolve.hpp>

namespace weave::detail {

// Private injection boundary for deterministic native completion/cancellation tests.
// Captured by value per query; no process-global backend override.
struct ResolverApi {
  decltype(&GetAddrInfoExW) query = GetAddrInfoExW;
  decltype(&GetAddrInfoExCancel) cancel = GetAddrInfoExCancel;
  decltype(&FreeAddrInfoExW) release = FreeAddrInfoExW;
};

Task<std::vector<Endpoint>> resolve_with(
  Context &context,
  std::string host,
  u16 port,
  ResolveOptions options,
  ResolverApi api);

} // namespace weave::detail
