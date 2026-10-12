# Weave

[![Windows](https://github.com/c-schembri/weave/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/c-schembri/weave/actions/workflows/windows.yml)

Async networking for C++23. Small APIs, explicit ownership, no exceptions.

**Experimental, not production-certified.** Windows IOCP and Linux io_uring.
No epoll or macOS backend.

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

`Task<T>` produces T or fails with std::error_code; awaited failures propagate
automatically. Synchronous setup and `run()` return `Result<T>` (std::expected).
`serve` owns concurrent clients; `on_data` owns their receive buffers and awaits
each callback before reading again.

Windows: VS 2022 C++ tools, CMake 3.25+, Git; Python 3.11+ for tests.

```sh
git clone https://github.com/c-schembri/weave.git
cd weave
cmake --preset windows-ci
cmake --build --preset ci-release --parallel 4
ctest --preset ci-release
./build/windows-ci/Release/echo_server_context_minimal.exe
```

[Linux/WSL requirements and installation](docs/build.md) /
[self-contained echo variants and other libraries](modules/tcp/examples/echo/README.md).

## Main APIs

**Execution:** Context runs on the calling thread; Runtime manages worker threads
and Contexts. Both expose create/run/spawn/detach.

```cpp
auto runtime = weave::Runtime::create({
  .workers = 4,
  .scheduler = weave::Scheduler::work_stealing,
});
if (!runtime)
  return weave::report_error(runtime.error());

auto result = runtime->run(server());
if (!result)
  return weave::report_error(result.error());
```

Include `<weave/runtime.hpp>` for Runtime. Spawn returns a join handle; detach
transfers ownership without one. `run` does not join unrelated tasks.
[Execution, ownership and cancellation](docs/execution.md).

**TCP:** Contextless setup uses the executing Task's Context.

```cpp
auto client = co_await weave::tcp::connect("localhost", 8080);
auto listener = co_await weave::tcp::listen("127.0.0.1", 8080);
auto accepted = co_await listener.accept({.no_delay = true});
```

Read/write Tasks are `read`, `read_exactly`, `write_all`; nonempty read returns
zero at EOF. IPv4/IPv6, asynchronous DNS and local sockets share this ownership model.
[TCP, local sockets and stream helpers](docs/networking.md).

**Recovery and deadlines:** explicitly inspect errors only where recovery is wanted.

```cpp
using namespace std::chrono_literals;

co_await weave::sleep_for(250ms);
auto result = co_await weave::as_result(weave::timeout(5s, client.read(buffer)));
if (!result)
  WEAVE_LOG_ERROR("Read: %s", result.error().message().c_str());
```

Include `<weave/timer.hpp>`. Cancellation is cooperative: cancel, then join/drain
before releasing borrowed streams/buffers. [Scopes and timers](docs/execution.md).

**PostgreSQL:** native protocol, no libpq dependency.

```cpp
auto pool = co_await weave::pg::Pool::create(options);
auto database = co_await pool.acquire();
auto rows = co_await database->execute("SELECT 42");
// The lease returns automatically at scope exit.
```

Include `<weave/postgres.hpp>`. Queries, prepared statements, batching, COPY,
streaming and blocking use share the same engine.
[PostgreSQL API and pooling](docs/postgres.md) / [libpq compatibility](docs/postgres-api-audit.md).

**Migrations:** explicit schema DSL, one C++ file per version, CMake-generated
registration without Python. [API and recovery](docs/migrations.md).

**JSON:** Glaze-backed struct conversion, synchronous checked results and reusable/fixed output buffers.
[JSON API](docs/json.md).

**TLS and synchronization:** verified OpenSSL streams, mTLS and explicit session/
revocation controls; bounded Channel and Semaphore for backpressure/admission.
[TLS](docs/tls.md) / [synchronization](docs/synchronization.md).

## Use In Your Project

Include/link only what you need. TCP does not require Runtime.

| Component | Header | CMake target |
| --- | --- | --- |
| Tasks, results, stream helpers | `<weave/core.hpp>`, `<weave/stream.hpp>` | `weave::core` |
| JSON struct conversion | `<weave/json.hpp>` | `weave::json` |
| Database storage, mapping and schema descriptions | `<weave/database.hpp>` | `weave::database` |
| Context and timers | `<weave/io.hpp>` | `weave::io` |
| Multicore runtime | `<weave/runtime.hpp>` | `weave::runtime` |
| TCP | `<weave/tcp.hpp>` | `weave::tcp` |
| Local sockets | `<weave/local.hpp>` | `weave::local` |
| Channels/semaphores | `<weave/sync.hpp>` | `weave::sync` |
| TLS (OpenSSL 3.5+) | `<weave/tls.hpp>` | `weave::tls` |
| PostgreSQL (also ICU 70+) | `<weave/postgres.hpp>` | `weave::postgres` |

```cmake
set(WEAVE_MODULES "tcp;runtime" CACHE STRING "" FORCE)
add_subdirectory(external/weave)
target_link_libraries(my_app PRIVATE weave::tcp weave::runtime)
```

Installed packages support `find_package(weave CONFIG REQUIRED COMPONENTS tcp)`.
[Build guide](docs/build.md) / [all documentation](docs/README.md).

## Local Runtime Benchmarks

Hosted CI runs correctness tests only. Local measurements reserve fixed separate
client cores and test 1/2/4/8/16/32 server cores where hardware permits.

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

Shared IOCP stays opt-in; default-layout throughput was essentially unchanged.
[Scaling, CPU/tail findings and reproduction](docs/runtime-scaling.md) /
[latest PostgreSQL/libpq measurements](docs/postgres-benchmarks.md).

## Development

Windows CI covers MSVC Debug, Release and AddressSanitizer. Benchmarks are manual;
raw evidence is uploaded separately, not tracked. [Testing](docs/testing.md) /
[architecture](docs/architecture.md) / [development rules](AGENTS.md).

No project license has been selected. Third-party dependencies retain their licenses.
