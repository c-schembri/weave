#pragma once

#include <weave/address.hpp>
#include <weave/io/context.hpp>
#include <string>
#include <vector>

namespace weave {

struct ResolveOptions {
  AddressFamily family = AddressFamily::any;
};

// Owns the UTF-8 host name. Numeric literals bypass DNS; result order follows the OS.
// Cancellation drains native completion before releasing the query or its buffers.
Task<std::vector<Endpoint>> resolve(Context &context, std::string host, u16 port, ResolveOptions options = {});
Task<std::vector<Endpoint>> resolve(std::string host, u16 port, ResolveOptions options = {});

} // namespace weave
