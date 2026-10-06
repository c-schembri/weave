#pragma once

#include "address.hpp"
#include <weave/resolve.hpp>
#include <netdb.h>

namespace weave::detail {

struct ResolverRequest {
  gaicb native{};
  void (*notify)(ResolverRequest &) noexcept = nullptr;
  void *state = nullptr;
};

// Per-query injection; no process-wide resolver override or external DNS fixture.
struct ResolverApi {
  void *state = nullptr;
  int (*start)(void *, ResolverRequest &) noexcept = nullptr;
  int (*cancel)(void *, ResolverRequest &) noexcept = nullptr;
  int (*status)(void *, ResolverRequest &) noexcept = nullptr;
  void (*release)(void *, addrinfo *) noexcept = nullptr;
};

Task<std::vector<Endpoint>> resolve_with(
  Context &context,
  std::string host,
  u16 port,
  ResolveOptions options,
  ResolverApi api);

} // namespace weave::detail
