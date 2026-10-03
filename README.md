# Weave

An experimental, exception-free C++23 async networking library. Performance
measurements come first; a small API follows. Windows IOCP works today. Linux
io_uring is planned, with no epoll fallback or macOS backend.

This is a first implementation, not production-ready networking infrastructure.

## Build and test

Requires Windows, Visual Studio 2022 with the C++ workload and a recent MSVC
toolset supporting `std::expected`, CMake 3.25+, and Git. Tested with MSVC 19.44.
The first configure downloads pinned doctest, Google Benchmark, and standalone
Asio sources. Subsequent builds use the local copies.

```powershell
cmake --preset windows
cmake --build --preset debug --parallel
ctest --preset debug
cmake --build --preset release --parallel
ctest --preset release
```

AddressSanitizer (requires the MSVC AddressSanitizer component):

```powershell
cmake --preset asan
cmake --build --preset asan --parallel
ctest --preset asan
```

CTest supplies the compiler runtime DLL path for ASan tests. Running an ASan
executable directly requires a Visual Studio developer shell or the equivalent
toolset runtime directory on PATH.

The compiled `weave::weave` CMake target has no third-party runtime dependency.
Use `-DWEAVE_BUILD_TESTS=OFF -DWEAVE_BUILD_BENCHMARKS=OFF` for a library-only
configure without dependency downloads. Examples can also be disabled with
`-DWEAVE_BUILD_EXAMPLES=OFF`. FetchContent source-directory overrides are
available for offline builds of tests and benchmarks.

## API

```cpp
#include <weave/weave.hpp>
#include <array>

int main() {
    weave::Context ctx;
    if (!ctx.status()) return 1;

    auto stream = ctx.block_on(ctx.connect("127.0.0.1", 8080));
    if (!stream) return 1;

    std::array<std::byte, 4> message{};
    auto sent = ctx.block_on(stream->write_all(message));
    if (!sent) return 1;
    auto received = ctx.block_on(stream->read_exactly(message));
    return received ? 0 : 1;
}
```

Run `build/windows/Debug/weave_echo.exe` in another terminal for the example
above. The echo example serves one client at a time; Ctrl+C terminates the
process rather than performing graceful shutdown.

- `Async<T>` is lazy, move-only, and single-consumer. Use `co_await` in a
  coroutine or `ctx.block_on(...)` in synchronous code. No implicit detaching.
- `Result<T>` is `std::expected<T, std::error_code>`. Network and setup errors
  are values. The library, tests, and benchmark clients disable exceptions.
- `listen()` is synchronous setup, returning `Result<TcpListener>`.
  `connect()`, `accept()`, `read()`, `read_exactly()`, and `write_all()` are async.
- A nonempty `read()` returns zero for EOF; an empty read returns zero without
  issuing I/O. `read_exactly()` reports early EOF as `connection_reset` for now.
- `when_all(Async<void>...)` starts a fixed group concurrently and joins every
  child. Each child handles its own errors. There is no implicit cancellation.
- `cancel()` requests cancellation of current operations. It does not mean
  they have finished; await/join them before closing or freeing buffers.
  Completion can still succeed if it wins the race. Cancellation is reported
  as Windows `ERROR_OPERATION_ABORTED` (995).
- `close()` is synchronous and refuses pending operations. Destructors close
  idle handles; destroying an active stream or suspended Async is a fatal
  contract violation, not implicit cancellation.

Contexts and their handles are confined to the constructing thread. A context
must outlive its handles. Streams may have one read and one write in flight at
once. Listeners currently allow one pending accept. Keep streams, buffers, and
borrowed arguments alive until their operations finish. Do not move a stream
while an operation borrows it. Nested `block_on()` is prohibited.

Out-of-memory and broken runtime invariants terminate; they are not recoverable
Results. Exception-free does not mean every conceivable failure is recoverable.

## Benchmarks

```powershell
powershell -ExecutionPolicy Bypass -File scripts/bench.ps1
```

This builds Release, runs tests, and writes Google Benchmark JSON plus machine
and Git metadata under `out/`. See [benchmark methodology](docs/benchmarks.md).
No speed advantage over Asio is claimed. CI benchmarks only check functionality;
hosted-runner timings are not performance gates.

## Current scope

Implemented: IPv4 numeric endpoints, AcceptEx/ConnectEx, overlapped receive/send,
partial-transfer loops, half-close, cancellation requests, bounded completion
batches, fixed structured joins, explicit error results, and loopback tests.

Not implemented: deadlines/timers, DNS, IPv6, dynamic task groups, cross-thread
scheduling, graceful server shutdown, Linux, TLS, or HTTP. Earlier API sketches
are design input, not a promise that these features already exist.

See [architecture](docs/architecture.md) and [development rules](AGENTS.md).
No project license has been selected yet; dependencies retain their own licenses.
