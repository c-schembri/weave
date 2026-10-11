#pragma once

#include <weave/address.hpp>
#include <weave/io/context.hpp>
#include <string>
#include <vector>

namespace weave {

/// Restrict asynchronous resolution to IPv4, IPv6, or both.
struct ResolveOptions {
  AddressFamily family = AddressFamily::any;
};

/// Resolve an owned UTF-8 host on the supplied Context, which must outlive completion.
/// An lvalue string is copied; an rvalue is moved. Numeric literals bypass DNS and result order
/// follows the OS. Cancellation drains native completion before releasing query storage.
Task<std::vector<Endpoint>> resolve(Context &context, std::string host, u16 port, ResolveOptions options = {});
/// Resolve an owned host using the active Context when the lazy task executes.
/// Execution outside a running Context is a contract violation; results own their endpoints.
Task<std::vector<Endpoint>> resolve(std::string host, u16 port, ResolveOptions options = {});

} // namespace weave
