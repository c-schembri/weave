# TCP Echo Examples

Self-contained sources: choose explicit stream loops or library-managed buffering,
then single-thread Context or four-worker work-stealing Runtime.

| Source | Target | API |
| --- | --- | --- |
| [minimal/context.cpp](minimal/context.cpp) | `echo_server_context_minimal` | serve/on_data, caller thread |
| [minimal/runtime.cpp](minimal/runtime.cpp) | `echo_server_runtime_minimal` | serve/on_data, four workers |
| [stream/context.cpp](stream/context.cpp) | `echo_server_stream_context` | accept/read/write, detached clients |
| [stream/runtime.cpp](stream/runtime.cpp) | `echo_server_stream_runtime` | Same stream API, four workers |

```sh
cmake --preset windows-ci
cmake --build --preset ci-release --parallel 4
./build/windows-ci/Release/echo_server_context_minimal.exe 8080
ctest --preset ci-release -R '^echo_server_.*_integration$'
```

Optional port defaults to 8080; zero requests an available port printed at startup.
Ctrl+C terminates the process, not graceful shutdown. Minimal serve scopes/drains
clients when its Task ends; stream examples detach them to Context/Runtime ownership.
Context variants need only weave::tcp, not Runtime.

## Library Comparison

| Source | Target | Natural API |
| --- | --- | --- |
| [comparisons/weave.cpp](comparisons/weave.cpp) | `echo_weave` | Task, serve/on_data |
| [comparisons/asio.cpp](comparisons/asio.cpp) | `echo_asio` | awaitable, redirect_error, co_spawn |
| [comparisons/libuv.cpp](comparisons/libuv.cpp) | `echo_libuv` | Session-owned handles/buffer, callbacks |
| [comparisons/usockets.cpp](comparisons/usockets.cpp) | `echo_usockets` | Socket-context callbacks, unsent FIFO |
| [comparisons/trantor.cpp](comparisons/trantor.cpp) | `echo_trantor` | Drogon's raw TCP layer, TcpServer/message buffer |

Shared contract: IPv4 loopback, one I/O worker, concurrent binary clients, partial
transfers, TCP_NODELAY and complete ordered echo before EOF after client half-close.
Resets affect one client. Startup errors exit 1; malformed arguments exit 2.
All use core's standard-only weave::parse_port; there is no common wrapper header.
Weave's main waits for one runtime worker; other loops run on main.

The shared test covers idle/concurrent clients, fragmentation, repeated exchanges,
half-close, 4 MiB slow-reader output, resets and startup failure. Weave/Asio/libuv/
uSockets pass. **Trantor 1.5.28 fails the backpressured half-close case**: upstream
[readCallback](https://github.com/an-tao/trantor/blob/63a4e5e164e219dc3bf30cdbfa1462ae5602fa97/trantor/net/inner/TcpConnectionImpl.cc#L136)
closes at EOF before its queued output drains. Its strict test remains enabled;
no upstream patch or expected-success exemption hides this difference.

Weave/Asio/libuv reuse per-client 4 KiB buffers and finish each write before reuse.
uSockets copies unsent borrowed data into a FIFO and retries on_writable; Trantor
owns its receive/output queues. These queues can grow with slow readers.
No overload/timeouts/graceful-process-shutdown/OOM recovery is promised.
This is an API comparison, not a benchmark.

Trantor is pinned separately and enables exceptions internally and in its example;
a top-level catch logs exceptions escaping setup/event loop. Native exit(1) cannot
be caught. Trantor also uses SOMAXCONN and a no-status TCP_NODELAY setter. The other
examples disable exceptions. Windows uSockets uses its libuv backend, but the
example uses uSockets APIs. Drogon HTTP/TLS dependencies are not fetched.

## Build Comparisons

Enable examples and benchmarks with tcp/runtime, as in the development preset.
Comparison dependencies never become Weave library dependencies or correctness-CI
requirements.

```sh
cmake --preset windows
cmake --build --preset release --target echo_weave echo_asio echo_libuv echo_usockets echo_trantor --parallel 4
./build/windows/examples/echo_servers/Release/echo_weave.exe 8080
ctest --preset release -R '^echo_(weave|asio|libuv|usockets|trantor)_integration$'
```

Run one server per port. The test owns only the processes it starts.
[Networking contracts](../../README.md).
