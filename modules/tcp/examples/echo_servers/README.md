# Echo server API comparison

Four implementations of the same raw TCP echo service, using the libraries'
actual APIs. No HTTP, TLS, benchmark adapters, or invented networking helpers.
The adjacent [echo.cpp](../echo.cpp) is the smaller, one-client-at-a-time example.

| Source | CMake target | Style |
| --- | --- | --- |
| [weave.cpp](weave.cpp) | `echo_weave` | Fallible `Task`, `as_result` recovery, runtime-owned sessions |
| [asio.cpp](asio.cpp) | `echo_asio` | `awaitable`, `as_tuple`, `co_spawn` |
| [libuv.cpp](libuv.cpp) | `echo_libuv` | Read/write callbacks, explicit handle/request lifetimes |
| [usockets.cpp](usockets.cpp) | `echo_usockets` | Socket-context callbacks, explicit unsent-byte queue |

## Shared behavior

- Bind to IPv4 loopback, `127.0.0.1:8080`, or the optional port argument.
  Port `0` asks the OS for an available port, printed after startup.
- One I/O worker, multiple concurrently connected clients; no thread per client.
  Weave's main thread waits for its single runtime worker; the others run their
  event loop on the main thread.
- Echo every byte in order, including binary data and partial TCP transfers.
  A read is not a message boundary. Clients may send continuously or repeatedly.
- Enable TCP_NODELAY. uSockets does this internally for accepted sockets.
- When the client half-closes its sending side, finish all queued echoes before
  sending EOF and closing that connection. A reset affects only that client.
- Use error values/callbacks, never operational exceptions. Startup errors exit
  with status 1; malformed arguments exit with status 2. Error text can differ.
- Keep serving until the process is terminated (for example, Ctrl+C). These are
  API examples, not graceful process-shutdown or production-server implementations.

`common.hpp` only parses the port and prints messages. All networking and
connection ownership are visible in each implementation. The non-Weave examples
do not include or link Weave.

The same wire behavior does not imply identical memory management: the coroutine
servers and libuv reuse a 4 KiB per-client buffer and finish its write before
reading again. uSockets delivers borrowed buffers in `on_data`; this example
copies unsent bytes into a per-client FIFO and retries from `on_writable`.
That queue can grow with a slow reader. There are no overload limits, timeouts,
or out-of-memory recovery guarantees. This is not a performance comparison.

On Windows the pinned uSockets library uses its libuv event backend. The example
still uses uSockets' own public API rather than calling libuv directly.

## Build and run

These targets reuse the existing pinned benchmark dependencies and are included
when both `WEAVE_BUILD_EXAMPLES` and `WEAVE_BUILD_BENCHMARKS` are ON and the
`tcp` and `runtime` modules are enabled, as in the development presets.
They are not enabled in a fresh library-only configure.
No comparison dependency is added to any Weave library component.

```powershell
cmake --preset windows
cmake --build --preset release --target echo_weave echo_asio echo_libuv echo_usockets --parallel

# Run one at a time on the default port, or give each a different port.
./build/windows/examples/echo_servers/Release/echo_weave.exe 8080
./build/windows/examples/echo_servers/Release/echo_asio.exe 8081
./build/windows/examples/echo_servers/Release/echo_libuv.exe 8082
./build/windows/examples/echo_servers/Release/echo_usockets.exe 8083

ctest --preset release -L examples
```

The same integration test runs against every executable: concurrent clients
while one remains idle, binary/fragmented payloads, repeated exchanges, a 4 MiB
slow-reader transfer, half-close/EOF, reset recovery, and startup failures. It
uses OS-assigned ports and terminates only the example processes it launches.
