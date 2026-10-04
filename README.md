# Weave

[![Windows](https://github.com/c-schembri/weave/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/c-schembri/weave/actions/workflows/windows.yml)

Straightforward async networking for C++23. Small APIs, explicit ownership, and
coroutines without exceptions.

```cpp
#include <weave/tcp.hpp>
#include <array>
#include <span>

weave::Task<void> echo(weave::TcpStream &client)
{
  std::array<std::byte, 4096> buffer;

  for (;;) {
    const auto n = co_await client.read(buffer);
    if (n == 0)
      co_return;
    co_await client.write_all(std::span(buffer).first(n));
  }
}
```

A `Task<T>` produces a value or fails with `std::error_code`. Participating
`co_await` expressions propagate errors automatically; use `as_result()` where
you want to recover. No exceptions or propagation macros.

**Experimental:** the API is evolving and is not production-ready. Windows IOCP
is implemented. Linux io_uring is planned; there is no Linux or macOS networking
backend today. No performance advantage over other libraries is claimed.

## Build and try

Requires Windows x64, Visual Studio 2022 with the C++ workload, CMake 3.25+, and
Git. Tested with MSVC 19.44. Core contains portable C++23; IO, TCP, and runtime
currently require Windows.

The correctness-only preset builds tests and examples, **not benchmarks**:

```powershell
git clone https://github.com/c-schembri/weave.git
cd weave
cmake --preset windows-ci
cmake --build --preset ci-debug --parallel 4
ctest --preset ci-debug
.\build\windows-ci\Debug\weave_echo.exe
```

The echo server listens on `127.0.0.1:8080` and handles one client at a time.
Ctrl+C terminates the process; this example does not implement graceful shutdown.
See the [complete source](modules/tcp/examples/echo.cpp).

Use `ci-release` instead of `ci-debug` to build and test Release. Test builds
download pinned doctest and standalone Asio sources. Library-only builds have
no third-party dependency downloads.

## Use in your project

Each feature has its own header and CMake target. Include and link only what
you need; TCP does not require the multicore runtime, and runtime does not
require TCP.

| Component | Include | CMake target | Dependencies |
| --- | --- | --- | --- |
| Tasks and results | `<weave/core.hpp>` | `weave::core` | None; header-only |
| Event loop | `<weave/io.hpp>` | `weave::io` | core |
| Worker runtime | `<weave/runtime.hpp>` | `weave::runtime` | io, core |
| TCP | `<weave/tcp.hpp>` | `weave::tcp` | io, core |

For a vendored build:

```cmake
set(WEAVE_MODULES tcp CACHE STRING "" FORCE)
add_subdirectory(external/weave)
target_link_libraries(my_app PRIVATE weave::tcp)
```

Or build and install the TCP component and its dependencies:

```powershell
cmake -S . -B build/tcp -DWEAVE_MODULES=tcp
cmake --build build/tcp --config Release
cmake --install build/tcp --config Release --prefix C:/Libraries/weave
```

Then set `CMAKE_PREFIX_PATH` to the installation and consume it:

```cmake
find_package(weave CONFIG REQUIRED COMPONENTS tcp)
target_link_libraries(my_app PRIVATE weave::tcp)
```

Select `-DWEAVE_MODULES="tcp;runtime"` and link both targets for networking on
runtime workers. Tests, examples, and benchmarks are separate opt-ins:
`WEAVE_BUILD_TESTS`, `WEAVE_BUILD_EXAMPLES`, and `WEAVE_BUILD_BENCHMARKS`.
All are off in a fresh library-only configure.

## Running async code

A `Context` is an I/O event loop, not a thread. `ctx.run(task)` drives it on
the calling thread until that task finishes and returns `Result<T>`:

```cpp
auto result = ctx.run(serve(*listener));
if (!result)
  return weave::report_error(result.error());
```

A `Runtime` creates worker threads, each with a context. `spawn` schedules a
task factory and returns `Result<JoinHandle<T>>`. The factory receives its
worker's context. Await the handle inside a coroutine, or use
`std::move(handle).get()` from synchronous non-runtime code.

Choose worker affinity (the default) or work stealing at runtime construction.
See [runtime semantics](docs/runtime.md) and the
[multicore example](modules/runtime/examples/multicore.cpp).

## Errors and lifetimes

- `Result<T>` is `std::expected<T, std::error_code>`. Setup operations and
  `Context::run` return results; asynchronous operations return tasks.
- `co_await as_result(operation)` exposes an error for explicit handling instead
  of propagating it out of the enclosing task.
- `Task<T>` is lazy, move-only, and single-consumer. `when_all` joins every
  child before propagating an error; it does not cancel siblings automatically.
- Keep contexts, streams, buffers, and borrowed arguments alive until their I/O
  completes. Streams support one pending read and one pending write. Standalone
  and worker-affine handles stay on their owning thread.
- Cancellation is a request, not completion. Await/join before releasing resources.
  Destroying active tasks or streams is a fatal contract violation, not implicit
  cancellation. Out-of-memory and broken invariants are also fatal.
- The runtime owns spawned work until completion. Dropping a join handle does
  not cancel its task or extend the lifetime of anything the task borrows.

See [Task contracts](docs/tasks.md), [runtime ownership](docs/runtime.md), and
[architecture](docs/architecture.md) for the complete model.

## Logging

`<weave/log.hpp>` is part of `weave::core`:

```cpp
WEAVE_LOG_INFO("listening on port %u", 8080u);
WEAVE_LOG_WARN("connection closed");
WEAVE_LOG_ERROR("read failed: %s", error.message().c_str());
WEAVE_LOG_DEBUG("received %zu bytes", buffer.size());
```

These are plain printf-style macros: literal format strings, a severity prefix,
and a trailing newline on stderr. DEBUG compiles out under `NDEBUG`, including
argument evaluation. Define `WEAVE_LOG_STREAM` before including the header to
select another `FILE*`. Logging is synchronous and best-effort.

`return weave::report_error(error);` prints the unprefixed error message to
stderr and returns `1`; it does not terminate the process. Its optional second
argument selects a `FILE*`. This is separate from coroutine `weave::fail(error)`.

## Tests and CI

[Windows CI](.github/workflows/windows.yml) runs MSVC Debug, Release, and AddressSanitizer on
pushes to `main`, pull requests, and manual dispatch. It covers module and
integration tests, isolated component builds, relocated install consumers, and
standalone public-header compilation.

CI explicitly sets `WEAVE_BUILD_BENCHMARKS=OFF` and excludes the `benchmark`
CTest label. **No benchmarks, benchmark smokes, or performance gates run
automatically.** The `windows-ci` and `asan` presets reproduce this setup locally.

For a local AddressSanitizer build, install the MSVC AddressSanitizer component:

```powershell
cmake --preset asan
cmake --build --preset asan --parallel 4
ctest --preset asan
```

CTest supplies the compiler runtime DLL path for ASan tests.

## Benchmarks

Benchmarks are an explicit, local workflow, separate from CI:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/bench.ps1
```

The full `windows` development preset enables comparisons against pinned Asio,
libuv, and uSockets. It also registers short benchmark correctness smokes with
CTest. Use `windows-ci` when you do not want any benchmark execution.

Workloads cover coroutine calls, TCP echo, bulk transfers, concurrent connections,
and multicore scheduling. The separate manual
[performance gate](docs/gate.md) compares throughput, CPU use, and tail latency.
Current performance parity is not established.

See [methodology](docs/benchmarks.md), [concurrent workloads](docs/concurrent.md),
and [historical results](benchmarks/results/). Timing results are evidence for
specific workloads and machines, not universal performance claims.

## Scope

Implemented: numeric IPv4 endpoints, async connect/accept/read/write, partial
transfers, half-close, cancellation, structured joins, and affine/work-stealing
worker scheduling.

Not implemented: DNS, IPv6, deadlines/timers, dynamic task groups, automatic
connection distribution, Linux networking, TLS, HTTP, or WebSockets.

Features live under `modules/<name>/`, including their tests, examples, and
benchmarks. Future protocols will have their own headers and link targets rather
than expanding a monolithic library. See [module boundaries](docs/architecture.md#module-boundaries),
[API migration](docs/migration.md), and [development rules](AGENTS.md).

## License

No project license has been selected yet. Third-party dependencies retain their
own licenses.
