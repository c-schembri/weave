# Weave

[![Windows](https://github.com/c-schembri/weave/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/c-schembri/weave/actions/workflows/windows.yml)

Async networking for C++23. Small APIs, explicit ownership, no exceptions.

**Experimental.** Windows IOCP is implemented; Linux io_uring is planned.
Not production-ready. No Linux or macOS networking backend yet.

## Start Here

A concurrent TCP echo server:

```cpp
#include <weave/tcp.hpp>
#include <weave/log.hpp>
#include <span>

int main()
{
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(weave::tcp::serve(
    "127.0.0.1",
    8080,
    {.no_delay = true},
    weave::tcp::on_data(
      [](weave::TcpStream &client, std::span<const std::byte> data) {
        return client.write_all(data);
      })));
  if (!result)
    return weave::report_error(result.error());
}
```

`Task<T>` produces `T` or fails with `std::error_code`. Awaited errors propagate
automatically; synchronous setup and `run()` return `Result<T>` (`std::expected`).
`tcp::serve` handles clients concurrently and owns their tasks. `on_data` manages
each client's receive buffer and waits for its callback before reading again.

Windows x64, Visual Studio 2022 C++ workload, CMake 3.25+, Git, and Python 3.11+
for tests. No Python or third-party dependencies for library-only builds.

```sh
git clone https://github.com/c-schembri/weave.git
cd weave
cmake --preset windows-ci
cmake --build --preset ci-release --parallel 4
ctest --preset ci-release
./build/windows-ci/Release/echo_server_context_minimal.exe
```

[Echo examples](modules/tcp/examples/echo/README.md) include explicit stream loops,
library-managed buffers, single-thread and multicore variants, and other libraries.

## Main APIs

**Execution.** A `Context` drives I/O on the calling thread. A `Runtime` manages
worker threads and contexts; replace the Context setup above for multicore execution:

```cpp
#include <weave/runtime.hpp>

auto runtime = weave::Runtime::create({
  .workers = 4, .scheduler = weave::Scheduler::work_stealing});
if (!runtime)
  return weave::report_error(runtime.error());

auto result = runtime->run(weave::tcp::serve(
  "127.0.0.1",
  8080,
  {.no_delay = true},
  weave::tcp::on_data(
    [](weave::TcpStream &client, std::span<const std::byte> data) {
      return client.write_all(data);
    })));
if (!result)
  return weave::report_error(result.error());
```

Use `spawn()` when you need a join handle, `detach()` when you do not. Inside a
task, `weave::detach()` selects the executing context/runtime. Outside one, use
`ctx->spawn(...)`, `ctx->detach(...)`, or the runtime equivalents.
[`run()` does not join independent tasks.](docs/context.md)

**TCP.** Inside a task, connect/listen use its executing context:

```cpp
auto client = co_await weave::tcp::connect("localhost", 8080);
auto listener = co_await weave::tcp::listen("127.0.0.1", 8080);
auto accepted = co_await listener.accept({.no_delay = true});
```

Streams provide `read()`, `read_exactly()`, and `write_all()`. A zero-length read
means EOF. IPv4, IPv6, and asynchronous DNS are supported.
[TCP guide](docs/tcp.md) / [Addresses and DNS](docs/addresses.md).

**Errors and timeouts.** Handle errors explicitly only where you want recovery:

```cpp
#include <weave/timer.hpp>
using namespace std::chrono_literals;

co_await weave::sleep_for(250ms);
auto result = co_await weave::as_result(weave::timeout(5s, client.read(buffer)));
if (!result)
  WEAVE_LOG_ERROR("Read: %s", result.error().message().c_str());
```

Cancellation is cooperative: cancel, then await/join before releasing borrowed
resources. Keep contexts, streams, buffers and arguments alive through completion.
[Tasks and lifetimes](docs/tasks.md) / [Cancellation, scopes and timers](docs/cancellation.md).

## Use In Your Project

Include and link only the components you need. TCP does not depend on Runtime.

| Component | Header | CMake target |
| --- | --- | --- |
| Tasks, results | `<weave/core.hpp>` | `weave::core` |
| Context, submission, timers | `<weave/io.hpp>` | `weave::io` |
| Multicore runtime | `<weave/runtime.hpp>` | `weave::runtime` |
| TCP streams and servers | `<weave/tcp.hpp>` | `weave::tcp` |

```cmake
set(WEAVE_MODULES "tcp;runtime" CACHE STRING "" FORCE)
add_subdirectory(external/weave)
target_link_libraries(my_app PRIVATE weave::tcp weave::runtime)
```

Installed packages support `find_package(weave CONFIG REQUIRED COMPONENTS tcp)`.
[Build and installation](docs/build.md) / [Module structure](docs/architecture.md) /
[Context](docs/context.md) / [Runtime](docs/runtime.md) / [Roadmap](docs/roadmap.md).

## Local Runtime Benchmarks

We measure runtime scaling locally with 1/2/4/8/16/32 server cores where hardware
permits, keeping client cores separate and fixed. Hosted CI runs correctness
tests only; benchmark evidence is uploaded separately, not committed as raw files.

<!-- benchmark-results:start -->
**Diagnostic run: timing validation failed in 1 measurement window(s).** All samples are retained. This is not a passed benchmark run; affected cohorts are non-comparable.

Local Windows run: 2026-10-06 17:23 +1100, AMD Ryzen 9 9900X 12-Core Processor. Source: [`606a343`](https://github.com/c-schembri/weave/commit/606a34393339cad154e339f5d2e9642c6f00ceea).

Workload: **1,024 clients / 1 KiB**, frame size each way.

7 x 2s per library/core count/workload. One worker per server core; 4 fixed, physically separate client cores. Median validated round trips/second; higher is better.

| Server cores | Weave | Asio | Tokio | Weave vs 1 core | Max throughput CV | Notes |
| ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 50,947 | 51,114 | 48,282 | 1.00x | 2.6% | within limits |
| 2 | 97,113 | 105,415 | 85,180 | 1.91x | 1.4% | within limits |
| 4 | 167,117 | 186,191 | 128,034 | 3.26x | 1.1% | client busy (Asio) |
| 8 | 211,466 | 175,883 | 145,571 | 4.17x | 0.8% | client busy (Weave, Asio) |
| 16 | N/A | N/A | N/A | N/A | N/A | insufficient physical cores |
| 32 | N/A | N/A | N/A | N/A | N/A | insufficient physical cores |

Closed-loop loopback results, not universal runtime rankings. Client-busy rows do not establish maximum server capacity; noisy comparisons are inconclusive for the affected metric.

[Full measurements and raw evidence](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261006-172307) / [methodology](https://github.com/c-schembri/weave/blob/main/docs/runtime-scaling.md).
<!-- benchmark-results:end -->

[Scaling methodology](docs/runtime-scaling.md) /
[Manual benchmark suites](docs/benchmarks.md).

## Development

Windows correctness CI runs MSVC Debug, Release, and AddressSanitizer tests;
performance comparisons run locally on suitable hardware. Raw benchmark results
are uploaded assets, not tracked source files. [Development rules](AGENTS.md).

No project license has been selected yet. Third-party dependencies retain their own licenses.
