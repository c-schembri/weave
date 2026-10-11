#pragma once

#include <weave/tcp/detail/serve.hpp>

namespace weave {

/// Listener and per-client socket policy for the structured serve convenience API.
struct ServeOptions {
  int backlog = 512;
  bool no_delay = false;
  bool ipv6_only = true;
};

namespace tcp {

/// Borrow listener and own handler/observer through all clients' completion and cancellation.
/// Handler takes an owned TcpStream and returns Task<void>; the optional observer is noexcept.
/// Client errors are isolated; accept/submission failure cancels and drains clients before return.
/// Shared callables may run concurrently on a stealing Runtime; captured references are not owned.
template <detail::TcpServeHandler F, detail::ErrorObserver H = detail::IgnoreError>
Task<void> serve(TcpListener &listener, AcceptOptions options, F handler, H on_error = {})
{
  return weave::scope(detail::TcpServeBody<F, H>{listener, options, std::move(handler), std::move(on_error)});
}

/// Own a listener on the active Context and serve until failure/cancellation, draining clients.
/// Owns handler/observer, not their reference captures. Port zero selects an ephemeral port.
template <detail::TcpServeHandler F, detail::ErrorObserver H = detail::IgnoreError>
Task<void> serve(Endpoint endpoint, ServeOptions options, F handler, H on_error = {})
{
  auto listener = co_await listen(endpoint, {.backlog = options.backlog, .ipv6_only = options.ipv6_only});
  co_await serve(listener, {.no_delay = options.no_delay}, std::move(handler), std::move(on_error));
}

/// Serve a numeric IP literal on the active Context; borrow address through lazy listener setup.
/// No DNS lookup or detached clients; owned handlers survive structured cancellation cleanup.
template <detail::TcpServeHandler F, detail::ErrorObserver H = detail::IgnoreError>
Task<void> serve(const char *address, u16 port, ServeOptions options, F handler, H on_error = {})
{
  detail::require(detail::current_context != nullptr);
  if (!address)
    co_await fail(std::errc::invalid_argument);
  auto endpoint = Endpoint::parse(address, port);
  if (!endpoint)
    co_await fail(endpoint.error());
  co_await serve(*endpoint, options, std::move(handler), std::move(on_error));
}

} // namespace tcp

} // namespace weave
