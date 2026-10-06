# TCP echo examples

Each example is self-contained. Choose library-managed receive buffering or explicit
stream operations, driven by either a single-threaded Context or a multicore Runtime.

| Source | CMake target | Approach |
| --- | --- | --- |
| [minimal/context.cpp](minimal/context.cpp) | `echo_server_context_minimal` | `tcp::serve` and `on_data`, calling thread |
| [minimal/runtime.cpp](minimal/runtime.cpp) | `echo_server_runtime_minimal` | `tcp::serve` and `on_data`, four work-stealing workers |
| [stream/context.cpp](stream/context.cpp) | `echo_server_stream_context` | Explicit accept/read loops and `detach`, calling thread |
| [stream/runtime.cpp](stream/runtime.cpp) | `echo_server_stream_runtime` | Explicit accept/read loops and `detach`, four work-stealing workers |

Context examples link only `weave::tcp`; Runtime examples also link `weave::runtime`.
They build with the correctness-only presets, without comparison dependencies:

```sh
cmake --preset windows-ci
cmake --build --preset ci-debug --target echo_server_context_minimal echo_server_runtime_minimal echo_server_stream_context echo_server_stream_runtime --parallel
./build/windows-ci/Debug/echo_server_context_minimal.exe 8080
ctest --preset ci-debug -R '^echo_server_.*_integration$'
```

All accept an optional port argument; `0` requests an available port, printed on
startup. Ctrl+C terminates the process; none implements graceful process shutdown.
The minimal servers scope and drain their clients before the server Task returns.
The stream servers detach clients to the Context/Runtime, which cancels and drains
them on shutdown instead.

## Library API comparison

Raw TCP echo implementations using the libraries' actual APIs. No HTTP, TLS,
benchmark adapters, or invented networking helpers. Weave, Asio, libuv and
uSockets pass the shared behavioral checks. Trantor is included as Drogon's TCP
layer, but currently fails the backpressured half-close check described below.
Each implementation uses its library's natural style, with required cleanup and
error handling intact. Callback state is kept local and visible rather than
hidden behind a shared wrapper.

| Source | CMake target | Style |
| --- | --- | --- |
| [comparisons/weave.cpp](comparisons/weave.cpp) | `echo_weave` | Fallible `Task`, `tcp::serve`, buffered `on_data` handler and scoped clients |
| [comparisons/asio.cpp](comparisons/asio.cpp) | `echo_asio` | `awaitable`, `redirect_error`, `co_spawn` |
| [comparisons/libuv.cpp](comparisons/libuv.cpp) | `echo_libuv` | Session-owned buffer and handles, named read/write callbacks |
| [comparisons/usockets.cpp](comparisons/usockets.cpp) | `echo_usockets` | Socket-context callbacks, explicit unsent-byte queue |
| [comparisons/trantor.cpp](comparisons/trantor.cpp) | `echo_trantor` | Drogon's TCP layer: `TcpServer`, message callbacks, library-owned buffers |

## Comparison contract

- Bind to IPv4 loopback, `127.0.0.1:8080`, or the optional port argument.
  Port `0` asks the OS for an available port, printed after startup.
- One I/O worker, multiple concurrently connected clients; no thread per client.
  Weave's main thread waits for its single runtime worker; the others run their
  event loop on the main thread.
- Echo every byte in order, including binary data and partial TCP transfers.
  A read is not a message boundary. Clients may send continuously or repeatedly.
- Enable TCP_NODELAY. uSockets does this internally for accepted sockets.
- When the client half-closes its sending side, finish all queued echoes before
  closing that connection, which sends EOF. No explicit send-side shutdown is
  needed here. A reset affects only that client.
- Use each library's native error handling: values/callbacks in the first four,
  plus a top-level exception handler in Trantor. Startup errors exit with status 1;
  malformed arguments exit with status 2. Error text can differ.
- Keep serving until the process is terminated (for example, Ctrl+C). These are
  API examples, not graceful process-shutdown or production-server implementations.

These are the requirements checked by the shared test, not a claim that Trantor
currently meets every requirement.

Each source shows startup, logging, networking, and connection ownership in one
file. All five use `weave::parse_port` from `<weave/port.hpp>` for argument parsing;
there is no shared example helper header to follow. The non-Weave examples need
only core's standard-library port utility, not Weave's IO, TCP, or runtime.
Trantor uses the core include path without linking its target, so it does not
inherit core's no-exceptions compile policy.

The same wire behavior does not imply identical memory management: the coroutine
servers and libuv reuse a 4 KiB per-client buffer and finish its write before
reading again. Weave's `tcp::on_data` owns that buffer and awaits the data handler
before reusing it; it adds no output queue or read-ahead. uSockets delivers borrowed
buffers in `on_data`; this example
copies unsent bytes into a per-client FIFO and retries from `on_writable`.
That queue can grow with a slow reader. There are no overload limits, timeouts,
or out-of-memory recovery guarantees. This is not a performance comparison.

At EOF, Weave and Asio return from the coroutine and release their owned socket.
libuv closes its session; reading resumes only after the previous write completes,
so EOF cannot discard an unfinished echo. uSockets closes only after its unsent
FIFO has drained. The extra callback state preserves the same echo-before-EOF
behavior; it is not an additional protocol or shutdown requirement.

On Windows the pinned uSockets library uses its libuv event backend. The example
still uses uSockets' own public API rather than calling libuv directly.

## Trantor and Drogon

[Drogon](https://github.com/drogonframework/drogon) is an HTTP framework. Its
[Trantor](https://github.com/an-tao/trantor) transport library is the appropriate
raw TCP comparison, not an HTTP handler accepting requests with bodies. This
example links only Trantor, pinned to v1.5.28
(`63a4e5e164e219dc3bf30cdbfa1462ae5602fa97`); it does not fetch Drogon or its HTTP
dependencies. TLS, c-ares, spdlog and upstream tests are disabled.

The example uses the main-thread event loop, enables TCP_NODELAY on new
connections, and echoes through `TcpConnection::send()`. Trantor owns the receive
buffer, copies unsent output into its internal queue, and handles connection
cleanup. No custom transport wrapper or manual send queue is added.

**Known parity failure:** v1.5.28 closes a connection immediately upon receiving
EOF in [`TcpConnectionImpl::readCallback()`](https://github.com/an-tao/trantor/blob/63a4e5e164e219dc3bf30cdbfa1462ae5602fa97/trantor/net/inner/TcpConnectionImpl.cc#L136),
even when its user-space send queue still holds echo data. The shared 4 MiB
slow-reader test sends FIN before draining the response and detects truncated
output. The strict `echo_trantor_integration` test is kept enabled and fails on
this case; it is not skipped, weakened, or marked as an expected success. The
other four pass. Trantor therefore is not fully functionally equivalent yet.

Its native API also differs in setup: listen uses `SOMAXCONN` rather than an
explicit backlog of 512, TCP_NODELAY configuration returns no status, and native
bind/listen failures log and exit with status 1. Its upstream implementation
uses exceptions internally. Both Trantor and its example enable exceptions; a
top-level `catch (const std::exception &)` logs exceptions escaping setup or the
event loop and returns status 1. All Weave code and the other examples remain
exception-disabled. Native `exit(1)` failures cannot be caught, and catching an
exception does not fix the half-close issue. No fetched source is patched.

## Build and run

The first four targets reuse existing pinned benchmark dependencies; Trantor is
fetched separately for its example only. These targets are included
when both `WEAVE_BUILD_EXAMPLES` and `WEAVE_BUILD_BENCHMARKS` are ON and the
`tcp` and `runtime` modules are enabled, as in the development presets.
They are not enabled in a fresh library-only configure.
No comparison dependency is added to any Weave library component.
Executable names and output paths remain unchanged from the previous layout;
`examples/echo_servers/` below is the build output directory, not the source directory.

```sh
cmake --preset windows
cmake --build --preset release --target echo_weave echo_asio echo_libuv echo_usockets echo_trantor --parallel

# Run one at a time on the default port, or give each a different port.
./build/windows/examples/echo_servers/Release/echo_weave.exe 8080
./build/windows/examples/echo_servers/Release/echo_asio.exe 8081
./build/windows/examples/echo_servers/Release/echo_libuv.exe 8082
./build/windows/examples/echo_servers/Release/echo_usockets.exe 8083
./build/windows/examples/echo_servers/Release/echo_trantor.exe 8084

# Strict comparison checks: Trantor currently fails the half-close case.
ctest --preset release -R '^echo_(weave|asio|libuv|usockets|trantor)_integration$'
```

The same Python 3.11+ integration test runs against every executable: concurrent clients
while one remains idle, binary/fragmented payloads, repeated exchanges, immediate
half-close before reading the echo, a 4 MiB slow-reader transfer, half-close/EOF,
reset recovery, and startup failures. It
uses OS-assigned ports and terminates only the example processes it launches.
Comparison examples are not part of the correctness-only CI presets; no
benchmark or comparison dependency is added to CI by this example.
