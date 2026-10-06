# Weave

[![Windows](https://github.com/c-schembri/weave/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/c-schembri/weave/actions/workflows/windows.yml)

Straightforward async networking for C++23. Small APIs, explicit ownership, and
coroutines without exceptions.

```cpp
#include <weave/tcp.hpp>
#include <array>
#include <span>
#include <utility>

weave::Task<void> echo(weave::TcpStream client)
{
  std::array<std::byte, 4096> buffer;

  while (auto received = co_await client.read(buffer))
    co_await client.write_all(std::span{buffer}.first(received));
}
```

A `Task<T>` produces a value or fails with `std::error_code`. Participating
`co_await` expressions propagate errors automatically; use `as_result()` where
you want to recover. No exceptions or propagation macros.

**Experimental:** the API is evolving and is not production-ready. Windows IOCP
is implemented. Linux io_uring is planned; there is no Linux or macOS networking
backend today. No performance advantage over other libraries is claimed.

See the [development priorities](docs/roadmap.md) for the hardening and feature sequence.

## Build and try

Requires Windows x64, Visual Studio 2022 with the C++ workload, CMake 3.25+, and
Git. Tested with MSVC 19.44. Core contains portable C++23; IO, TCP, and runtime
currently require Windows.

Tests and automation also require **Python 3.11+**, with no pip packages.
Library-only builds and consumers do not need Python.

The correctness-only preset builds tests and examples, **not benchmarks**:

```sh
git clone https://github.com/c-schembri/weave.git
cd weave
cmake --preset windows-ci
cmake --build --preset ci-debug --parallel 4
ctest --preset ci-debug
./build/windows-ci/Debug/echo_server_context_minimal.exe
```

All four echo examples handle clients concurrently, with a task, socket, and buffer per
connection. An idle, slow, or disconnected client does not stop the others:

| Example | Receive handling | Execution |
| --- | --- | --- |
| [echo_server_context_minimal](modules/tcp/examples/echo/minimal/context.cpp) | `on_data`, library-owned buffer | Calling thread |
| [echo_server_runtime_minimal](modules/tcp/examples/echo/minimal/runtime.cpp) | `on_data`, library-owned buffer | Four work-stealing workers |
| [echo_server_stream_context](modules/tcp/examples/echo/stream/context.cpp) | Explicit stream read loop | Calling thread |
| [echo_server_stream_runtime](modules/tcp/examples/echo/stream/runtime.cpp) | Explicit stream read loop | Four work-stealing workers |

See the [echo family guide](modules/tcp/examples/echo/README.md) for builds and library comparisons.

Context examples link only `weave::tcp`; Runtime examples also link `weave::runtime`.
All listen on `127.0.0.1:8080` by default. An optional port argument overrides
`8080`; use `0` for an available port, printed after startup. Ctrl+C terminates
the process; these examples do not implement graceful process shutdown.

For a concurrent server, pass the connection handler directly:

```cpp
auto result = ctx->run(weave::tcp::serve(
  "127.0.0.1", 8080, {.backlog = 512, .no_delay = true}, echo));
if (!result)
  return weave::report_error(result.error());
```

`tcp::serve` owns its client tasks. Cancellation stops acceptance, cancels clients,
and drains them before returning. Client failures do not stop acceptance; an optional
`noexcept` error callback observes them. See [TCP servers](docs/tcp.md).

For library-managed receive buffering, adapt a data handler:

```cpp
weave::Task<void> echo_data(weave::TcpStream &client, std::span<const std::byte> data)
{
  co_await client.write_all(data);
}

// Inside a Task:
co_await weave::tcp::serve(
  "127.0.0.1", port, {.no_delay = true}, weave::tcp::on_data(echo_data));
```

The adapter awaits each handler before reusing its buffer. The slice is borrowed
until the handler finishes; TCP chunks are not application message boundaries.

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
| Port parsing | `<weave/port.hpp>` | `weave::core` | Standard library only; header-only |
| Context, event loop, task submission and joins | `<weave/io.hpp>` | `weave::io` | core |
| Worker runtime | `<weave/runtime.hpp>` | `weave::runtime` | io, core |
| TCP | `<weave/tcp.hpp>` | `weave::tcp` | io, core |
| Concurrent TCP servers | `<weave/tcp/serve.hpp>` | `weave::tcp` | io, core |
| Buffered data handlers | `<weave/tcp/on_data.hpp>` | `weave::tcp` | io, core |

`weave::parse_port(std::string_view)` returns
`std::expected<std::uint16_t, std::error_code>`. It accepts decimal ports from
`0` through `65535`, including leading zeros. Empty input, signs, whitespace,
trailing characters, and overflow return `std::errc::invalid_argument`.
The header uses only the standard library; argument handling and default ports
remain application choices.

For a vendored build:

```cmake
set(WEAVE_MODULES tcp CACHE STRING "" FORCE)
add_subdirectory(external/weave)
target_link_libraries(my_app PRIVATE weave::tcp)
```

Or build and install the TCP component and its dependencies:

```sh
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

A `Context` is an I/O event loop, not a thread. Create it with a checked result;
`run(task)` drives it on the calling thread until that task finishes. For one
connection, using `echo()` above:

```cpp
auto ctx = weave::Context::create();
if (!ctx)
  return weave::report_error(ctx.error());

auto listener = weave::tcp::listen(*ctx, "127.0.0.1", 8080);
if (!listener)
  return weave::report_error(listener.error());

auto client = ctx->run(listener->accept());
if (!client)
  return weave::report_error(client.error());

auto result = ctx->run(echo(std::move(*client)));
if (!result)
  return weave::report_error(result.error());
```

`create(options)` returns `Result<Context>` with either a usable event loop or
the IOCP setup error. The context is constructed directly inside the result:
neither is movable, because sockets and continuations borrow its address.
Use `ctx->` to call methods and `*ctx` when passing a `Context &`. Keep the result
alive until its sockets and tasks have been destroyed. There is no separate
`status()` check or extra allocation for the result wrapper.

`tcp::listen()` resolves and caches the bound port during setup, including an
OS-assigned port when requesting `0`. Query failures are returned by `listen()`.
`listener.local_port()` is a plain `u16` accessor: no `Result`, `co_await`, or
socket query. The cached port remains available after closing the listener.

Configure accepted streams directly when needed:

```cpp
auto client = co_await listener.accept({.no_delay = true});
```

`accept()` and `accept({.no_delay = false})` retain the default TCP behavior
(Nagle's algorithm enabled). With `true`, `TCP_NODELAY` is set before returning
the stream. If configuration fails, the accepted socket is closed and the Task
fails with the configuration error.

Synchronous operations return ordinary values or `Result<T>`, never awaitables.
For example, `no_delay()`, `shutdown_send()`, `cancel()`, `close()`, and `spawn()`
must be called and checked directly. `co_await Result<T>` is rejected by the compiler.
Inside a Task, propagate a synchronous error explicitly:

```cpp
if (auto status = client.no_delay(); !status)
  co_await weave::fail(status.error());
```

`fail(error)` routes failure out of a Task; it does not perform asynchronous work.
Await socket I/O, timers, and join handles instead.

Inside a task, TCP setup can use the executing Context automatically:

```cpp
auto listener = co_await weave::tcp::listen("127.0.0.1", 8080);
auto client = co_await weave::tcp::connect("127.0.0.1", 8080);
```

These overloads return lazy Tasks: they choose the active Context when executed,
not when constructed. They work with standalone Contexts, Runtime workers, and
custom runtimes driving Context. Starting them without an active Context is a
fatal contract violation. Endpoint strings are borrowed through setup. Explicit
Context overloads remain available; `listen(ctx, ...)` is synchronous and returns
`Result<TcpListener>`. Existing sockets retain their original Context binding.

A `Runtime` creates worker threads, each with a context. Create it with a checked
result, just like Context:

```cpp
auto runtime = weave::Runtime::create({.workers = 4, .scheduler = weave::Scheduler::work_stealing});
if (!runtime)
  return weave::report_error(runtime.error());

auto result = runtime->run(serve(*port));
if (!result)
  return weave::report_error(result.error());
```

`Runtime::create()` waits for worker startup and returns its error after cleaning
up started workers. The runtime and its owning result are immovable; use `runtime->`
and pass `*runtime` to functions taking `Runtime &`. There is no separate `status()`.
`run(task_or_factory)` schedules on workers and blocks only until that root finishes.
It leaves submissions open and does not join independent tasks. Runtime destruction
cancels and drains them, so the echo example does not need an explicit `shutdown()`.

Runtime scheduling and I/O layout are independent. The default
`IoLayout::sharded` uses one IOCP per worker; opt into `.io_layout =
weave::IoLayout::shared` for one runtime-owned IOCP serviced by all workers.
Affinity and task serialization are unchanged. Shared IOCP is an experimental
candidate, not a claimed performance win; see [runtime details](docs/runtime.md)
and the [manual layout comparison](docs/benchmarks.md#shared-versus-sharded-iocp).

Both Runtime and Context
accept `spawn(task)` or `spawn(factory)` and return `Result<JoinHandle<T>>`.
A factory may take no arguments or `Context &` and must return a `Task<T>` by value.
Factories run on the selected execution thread; a direct task transfers its
already-created frame without running its body on the caller. Both forms retain
ownership until completion, even if the join handle is discarded. Await the handle
inside a coroutine, or use `std::move(handle).get()` from a thread that is not driving it.

Use `detach(task)` or `detach(factory)` when no join handle is wanted. It returns
`void`, without `co_await`, and deliberately discards errors. Add `on_error` to
report both submission rejection and eventual task failure. The optional handler
must return void, be `noexcept`, and support non-throwing moves. Rejection is
reported synchronously on the submitting thread; task errors on the execution
thread after coroutine cleanup. Success does not invoke the handler.
Context and Runtime own execution in both cases and discard detached return values
on an execution thread. Runtime also has `spawn_on(worker, task_or_factory)` and
`detach_on(worker, task_or_factory, on_error)` for pinned work. Both echo servers
use an inline error handler and submit each owning client task directly:

```cpp
weave::detach(echo(std::move(client)), [](std::error_code error) noexcept {
  WEAVE_LOG_ERROR("Client: %s", error.message().c_str());
});
```

`weave::detach()` inherits the executing scope. Standalone Contexts and
worker-affine runtimes keep the child on the current Context; work-stealing
runtimes own an independent, stealable child. Neither echo coroutine needs a
Context or Runtime parameter. Outside a running scope, use `ctx->detach(...)`
or `runtime->detach(...)` to select the destination; free detach otherwise fails
its contract. `<weave/io/detach.hpp>` also provides the free function directly.

Before asynchronous TCP operations, Weave checks that execution matches the
socket's Context or is scheduler-routed within the same work-stealing runtime.
Using Context A's socket while driving an unrelated Context B fails a contract,
even when both belong to the same thread. Synchronous setup and cleanup remain
valid outside `run()`. Two Contexts on one thread may be driven separately.

Use `std::move(task)` when submitting a named Task variable. Direct tasks own
their frames and value parameters, not borrowed references or the closure of a
temporary capturing coroutine lambda. Submit the factory itself when its closure
must stay alive or construction needs to happen on the execution thread.

`Context::run(task)` waits only for that task, not independent spawned work.
The no-argument `Context::run()` serves submissions until `request_stop()`, even
while idle, then drains owned tasks. `shutdown()` requests cancellation and
drains on the owning thread; destruction does the same. Custom runtimes can
manage threads and contexts using these public APIs without `weave::runtime`.
See [Context execution and ownership](docs/context.md).

Choose worker affinity (the default) or work stealing at runtime construction.
See [runtime semantics](docs/runtime.md) and the
[multicore example](modules/runtime/examples/multicore.cpp).

## Cancellation and timers

```cpp
#include <weave/timer.hpp>

using namespace std::chrono_literals;

// Inside a Task:
co_await weave::sleep_for(250ms);
auto bytes = co_await weave::timeout(5s, client.read(buffer));

// Individual or grouped task cancellation:
job.cancel();
weave::CancelSource stop;
runtime->detach(serve(), {.cancel = stop.token()}, log_error);
stop.cancel();
```

Timers use the event loop, not extra threads. A timeout cancels and drains its
operation before returning `std::errc::timed_out`; buffers remain alive throughout.
`weave::scope(body)` owns and drains a group of children. See
[cancellation, scopes and timers](docs/cancellation.md) for inheritance,
cooperative cancellation, and the C++ borrowed-lifetime boundaries.

## Errors and lifetimes

- `Result<T>` is `std::expected<T, std::error_code>`. `Context::create`, other setup
  operations, `Runtime::create`, and both `run` methods return results;
  asynchronous operations return tasks.
- `co_await as_result(operation)` exposes an error for explicit handling instead
  of propagating it out of the enclosing task.
- `operation.on_error(handler)` returns an owning Task adapter that observes failure
  without changing the result. The handler is a synchronous `noexcept` callable
  returning void, such as a logging function; it is not called on success.
- `Task<T>` is lazy, move-only, and single-consumer. `when_all` joins every
  child before propagating an error; it does not cancel siblings automatically.
- Keep contexts, streams, buffers, and borrowed arguments alive until their I/O
  completes. Streams support one pending read and one pending write. Standalone
  and worker-affine handles stay on their owning thread.
- Cancellation is a request, not completion. Await/join before releasing resources.
  Destroying active tasks or streams is a fatal contract violation, not implicit
  cancellation. Out-of-memory and broken invariants are also fatal.
- The context or runtime owns spawned work until completion. Dropping a join handle does
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
standalone public-header compilation. Python tooling tests cover synthetic gate
decisions, archived-analysis compatibility, and process-tree timeout cleanup.

CI explicitly sets `WEAVE_BUILD_BENCHMARKS=OFF` and excludes the `benchmark`
CTest label. **No benchmarks, benchmark smokes, or performance gates run
automatically.** The `windows-ci` and `asan` presets reproduce this setup locally.

For a local AddressSanitizer build, install the MSVC AddressSanitizer component:

```sh
cmake --preset asan
cmake --build --preset asan --parallel 4
ctest --preset asan
```

CTest supplies the compiler runtime DLL path for ASan tests.

## Benchmarks

Benchmarks are an explicit, local workflow, separate from CI:

```sh
python scripts/bench.py
```

The full `windows` development preset enables comparisons against pinned Asio,
libuv, and uSockets. It also registers short benchmark correctness smokes with
CTest. Use `windows-ci` when you do not want any benchmark execution.

Workloads cover coroutine calls, TCP echo, bulk transfers, concurrent connections,
and multicore scheduling. The separate manual
[performance gate](docs/gate.md) compares throughput, CPU use, and tail latency.
Current performance parity is not established.

An optional [four-worker Weave/Tokio/Asio stress comparison](modules/tcp/benchmarks/tokio/README.md)
uses separate server/client processes, up to 4096 active connections, large frames,
and uneven CPU work. Rust is needed only for this manually built benchmark.

See [methodology](docs/benchmarks.md) and [concurrent workloads](docs/concurrent.md).
Benchmark results are local artifacts, ignored by Git under `benchmarks/results/`.
Publish raw evidence separately when sharing a report. Timing results are evidence
for specific workloads and machines, not universal performance claims.

## Scope

Implemented: numeric IPv4 endpoints, async connect/accept/read/write, partial
transfers, half-close, per-task/group cancellation, structured scopes, awaitable
timers/timeouts, and affine/work-stealing worker scheduling. See
[cancellation and timers](docs/cancellation.md) for the APIs and lifetime contracts.

Not implemented: DNS, IPv6, automatic
connection distribution, Linux networking, TLS, HTTP, or WebSockets.

Features live under `modules/<name>/`, including their tests, examples, and
benchmarks. Future protocols will have their own headers and link targets rather
than expanding a monolithic library. See [module boundaries](docs/architecture.md#module-boundaries),
[API migration](docs/migration.md), and [development rules](AGENTS.md).

## License

No project license has been selected yet. Third-party dependencies retain their
own licenses.
