#pragma once

#include <weave/tcp/detail/serve.hpp>

namespace weave {

struct ServeOptions {
  int backlog = 512;
  bool no_delay = false;
};

namespace tcp {

// Borrows listener until completion. Owns handlers and drains clients before returning.
// Handler and error callback may be invoked concurrently on a work-stealing Runtime.
// Client errors are observed and isolated; accept/submission errors fail the server.
template <detail::TcpServeHandler F, detail::ErrorObserver H = detail::IgnoreError>
Task<void> serve(TcpListener &listener, AcceptOptions options, F handler, H on_error = {})
{
  return weave::scope(detail::TcpServeBody<F, H>{listener, options, std::move(handler), std::move(on_error)});
}

// Lazy setup on the executing Context. ipv4 must remain alive until setup finishes.
template <detail::TcpServeHandler F, detail::ErrorObserver H = detail::IgnoreError>
Task<void> serve(const char *ipv4, u16 port, ServeOptions options, F handler, H on_error = {})
{
  auto listener = co_await listen(ipv4, port, options.backlog);
  co_await serve(listener, {.no_delay = options.no_delay}, std::move(handler), std::move(on_error));
}

} // namespace tcp

} // namespace weave
