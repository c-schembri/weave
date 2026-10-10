# Weave

[![Windows](https://github.com/c-schembri/weave/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/c-schembri/weave/actions/workflows/windows.yml)

Async networking for C++23. Small APIs, explicit ownership, no exceptions.

**Experimental.** Windows IOCP and Linux io_uring backends are implemented.
Not production-ready. No epoll or macOS backend.

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
for tests. Linux uses GCC 14+ with liburing 2.3+ and a kernel supporting io_uring.
Python is not needed for library-only builds. TLS needs OpenSSL 3.5+;
PostgreSQL also needs ICU 70+.
[Linux/WSL setup and current limitations](docs/linux.md).

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

**Local sockets.** `local::connect()` and `local::listen()` use the same Task and
stream model for filesystem sockets on Windows/Linux and abstract sockets on Linux.
[Local sockets and peer credentials](docs/local.md).

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

**Synchronization.** Bounded `Channel<T>` provides async send/receive with
backpressure; `Semaphore` limits concurrent work with RAII permits. Both support
cancellation and cross-context wakeups. [Guide and example](docs/synchronization.md).

**Streams and TLS.** Small stream concepts let helpers work over TCP, TLS, and
custom transports. Optional OpenSSL-backed TLS has verified client/server
handshakes, mTLS, explicit session/revocation policy and the same read/write API.
[Stream helpers](docs/streams.md) /
[TLS setup, example, and limitations](docs/tls.md).

**PostgreSQL.** Native protocol Tasks cover verified connections, queries,
prepared statements, batching, row streaming and COPY. A blocking facade drives
the same engine. The implemented scope is experimental, not drop-in libpq parity.
[API and examples](docs/postgres.md) / [Parity checklist](docs/postgres-parity.md) /
[libpq benchmarks](docs/postgres-benchmarks.md).

[TLS modes and explicit libpq configuration migration](docs/postgres-connections.md#explicit-libpq-migration-profile) /
[Connection diagnostics](docs/postgres-diagnostics.md#connection-reports).

[Supported scope and deployment boundaries](docs/postgres-release.md).

Latest native Windows PostgreSQL run (10 October 2026), four workers / 32 connections:

| Plaintext workload | Weave affine ops/s | libpq ops/s |
| --- | ---: | ---: |
| Simple query | 105,239 | 114,188 |
| Prepared query | 113,959 | 123,078 |
| Batch statements | 528,713 | 535,137 |
| Row-heavy query | 27,897 | 33,266 |

Seven sequential samples; throughput spread 1.9-7.9%. This is not a uniform win.
[TLS, both schedulers, tail latency and memory](docs/postgres-benchmarks.md) /
[Raw evidence and exact working-tree provenance](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261010-postgres-final).

## Use In Your Project

Include and link only the components you need. TCP does not depend on Runtime.

| Component | Header | CMake target |
| --- | --- | --- |
| Tasks, results | `<weave/core.hpp>` | `weave::core` |
| Context, submission, timers | `<weave/io.hpp>` | `weave::io` |
| Multicore runtime | `<weave/runtime.hpp>` | `weave::runtime` |
| TCP streams and servers | `<weave/tcp.hpp>` | `weave::tcp` |
| Local socket streams | `<weave/local.hpp>` | `weave::local` |
| Channels and semaphores | `<weave/sync.hpp>` | `weave::sync` |
| Stream concepts and helpers | `<weave/stream.hpp>` | `weave::core` |
| Verified TLS streams (OpenSSL 3.5+) | `<weave/tls.hpp>` | `weave::tls` |
| Native PostgreSQL client (experimental) | `<weave/postgres.hpp>` | `weave::postgres` |

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
Local Windows run: 2026-10-06 18:26 +1100, AMD Ryzen 9 9900X 12-Core Processor. Source: [`dd0d265`](https://github.com/c-schembri/weave/commit/dd0d265e711ec31580daf93715ef8b547da4f615).

Before: [`606a343`](https://github.com/c-schembri/weave/commit/606a34393339cad154e339f5d2e9642c6f00ceea); the archived before binary is replayed within every matched repetition block.

Workload: **1,024 clients / 1 KiB**, frame size each way.

7 x 2s per library/core count/workload. One worker per server core; 4 fixed, physically separate client cores. Median validated round trips/second; higher is better.

Paired change is the median of within-repetition ratios, not the ratio of the displayed medians.

| Server cores | Weave before | Weave after | Paired change | Asio | Tokio | Max throughput CV | Notes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| 1 | 50,151 | 49,998 | -0.5% | 49,739 | 47,367 | 1.2% | within limits |
| 2 | 94,858 | 94,554 | -0.4% | 102,202 | 83,494 | 1.1% | within limits |
| 4 | 165,161 | 163,639 | -0.2% | 185,469 | 126,276 | 0.9% | client busy (Asio) |
| 8 | 209,353 | 210,022 | +0.5% | 174,586 | 142,542 | 0.6% | client busy (Weave, Asio, Weave before) |
| 16 | N/A | N/A | N/A | N/A | N/A | N/A | insufficient physical cores |
| 32 | N/A | N/A | N/A | N/A | N/A | N/A | insufficient physical cores |

Closed-loop loopback results, not universal runtime rankings. Client-busy rows do not establish maximum server capacity; noisy comparisons are inconclusive for the affected metric.

[Full measurements and raw evidence](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261006-182604) / [methodology](https://github.com/c-schembri/weave/blob/main/docs/runtime-scaling.md).
<!-- benchmark-results:end -->

All 560 windows passed timing validation. Default-layout throughput is essentially
unchanged. Shared IOCP improves this workload at four cores but regresses at eight;
it remains opt-in. [Optimization findings and CPU/tail results](docs/runtime-performance.md).

[Scaling methodology](docs/runtime-scaling.md) /
[Manual benchmark suites](docs/benchmarks.md).

## Development

Windows correctness CI runs MSVC Debug, Release, and AddressSanitizer tests;
performance comparisons run locally on suitable hardware. Raw benchmark results
are uploaded assets, not tracked source files. [Development rules](AGENTS.md).

No project license has been selected yet. Third-party dependencies retain their own licenses.
