# Weave

[![Windows](https://github.com/c-schembri/weave/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/c-schembri/weave/actions/workflows/windows.yml)
[![Benchmarks](https://github.com/c-schembri/weave/actions/workflows/benchmarks-windows.yml/badge.svg?branch=main)](https://github.com/c-schembri/weave/actions/workflows/benchmarks-windows.yml)

Async networking for C++23. Small APIs, explicit ownership, no exceptions.

**Experimental.** Windows IOCP is implemented; Linux io_uring is planned.
Not production-ready. No Linux or macOS networking backend yet.

## Start Here

A concurrent TCP echo server:

```cpp
#include <weave/tcp.hpp>
#include <weave/log.hpp>
#include <array>
#include <span>

weave::Task<void> echo(weave::TcpStream client)
{
  std::array<std::byte, 4096> buffer;

  while (auto received = co_await client.read(buffer))
    co_await client.write_all(std::span{buffer}.first(received));
}

int main()
{
  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(weave::tcp::serve(
    "127.0.0.1", 8080, {.no_delay = true}, echo));
  if (!result)
    return weave::report_error(result.error());
}
```

`Task<T>` produces `T` or fails with `std::error_code`. Awaited errors propagate
automatically; synchronous setup and `run()` return `Result<T>` (`std::expected`).
`tcp::serve` handles clients concurrently and owns their tasks.

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
  "127.0.0.1", 8080, {.no_delay = true}, echo));
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

## Latest Windows Benchmarks

<!-- benchmark-results:start -->
Latest complete run: [`855f392`](https://github.com/c-schembri/weave/commit/855f392a781dbf42a7c41e8598874ed66d2baa86), 2026-10-06 05:43 +0000; [full results and raw evidence](https://github.com/c-schembri/weave/actions/runs/37419857460).

Windows x64 / AMD EPYC 7763 64-Core Processor; server/client workers: 1/1, on separate cores. 7 x 1s per library/workload. Median round trips/second; higher is better.

| Workload | Weave | Asio | Tokio | Max throughput CV | Notes |
| --- | ---: | ---: | ---: | ---: | --- |
| 64 clients / 1 KiB | 115,254 | 122,117 | 112,684 | 3.7% | client busy |
| 1,024 clients / 1 KiB | 115,676 | 115,905 | 95,290 | 3.3% | client busy |
| 64 clients / 64 KiB | 38,606 | 37,220 | 51,671 | 5.9% | p99 noisy; client busy |
| 256 clients / uneven CPU | 79,045 | 74,890 | 64,412 | 3.1% | p99 noisy |

Throughput variation is within limits. **2 library/workload p99 measurements are noisy; tail-latency comparisons involving them are inconclusive.**

Client busy: at least one backend's load generator used >=90% of its core budget. These are end-to-end loopback results, not maximum server capacity.

CPU use, p99/p99.9 latency, paired confidence intervals, and limitations: [benchmark protocol](docs/ci-benchmarks.md).
<!-- benchmark-results:end -->

[CI protocol and limitations](docs/ci-benchmarks.md) /
[Latest workflow runs](https://github.com/c-schembri/weave/actions/workflows/benchmarks-windows.yml) /
[Manual benchmark suites](docs/benchmarks.md).

## Development

Windows correctness CI runs MSVC Debug, Release, and AddressSanitizer tests;
performance comparisons run in a separate bounded workflow. Raw benchmark results
are artifacts, not tracked source files. [Development rules](AGENTS.md).

No project license has been selected yet. Third-party dependencies retain their own licenses.
