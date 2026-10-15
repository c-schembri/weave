# Weave

[![Windows](https://github.com/c-schembri/weave/actions/workflows/windows.yml/badge.svg?branch=main)](https://github.com/c-schembri/weave/actions/workflows/windows.yml)

Modular C++23 libraries for asynchronous services: execution, networking,
database access and data handling. Small APIs, explicit ownership, no exceptions.

**Experimental, not production-certified.** Windows IOCP and Linux io_uring.
No epoll or macOS backend.

[Features and versions](docs/features.md) |
[Build and install](docs/build.md) |
[Documentation](docs/README.md)

## Implemented

| Area | Available | Guides |
| --- | --- | --- |
| Execution | Coroutine Tasks, checked results, Context, multicore Runtime, scopes, cancellation and timers | [Execution](modules/io/README.md) |
| Networking | TCP clients/servers, IPv4/IPv6, asynchronous DNS and local sockets | [Networking](modules/tcp/README.md) |
| Security | Verified TLS, mutual TLS, ALPN, sessions and revocation policies | [TLS](modules/tls/README.md) |
| Synchronization | Bounded channels and semaphores | [Synchronization](modules/sync/README.md) |
| Databases | Native PostgreSQL/MySQL clients, pooling, row/struct mapping and schema migrations | [PostgreSQL](modules/postgres/README.md), [MySQL](modules/mysql/README.md), [migrations](modules/database/docs/migrations.md) |
| Data | Glaze-backed JSON serialization and deserialization | [JSON](modules/json/README.md) |

Async operations return `Task<T>`; fallible synchronous operations return `Result<T>`.
Context drives the calling thread; Runtime adds managed worker threads. Modules are
independent where possible: TCP does not require Runtime, and database clients do
not require libpq or libmysqlclient.

## Planned

| Area | Targets |
| --- | --- |
| Networking | UDP, QUIC, HTTP/1.1, HTTP/2, HTTP/3 and WebSocket |
| APIs | GraphQL clients, server execution and subscriptions; gRPC |
| Databases | Additional SQL, NoSQL, graph, search and analytics vendors; compatibility qualification |
| Messaging | Kafka, NATS/JetStream, RabbitMQ, AMQP, MQTT, Pulsar and cloud messaging |
| Data | Protobuf, Avro, BSON, MessagePack and Schema Registry integration |
| Storage | S3-compatible storage, Azure Blob Storage and Google Cloud Storage |
| Execution and tooling | Blocking-work executor, asynchronous files and consistent metrics/tracing |
| Releases | Linux CI, protocol fuzzing/interoperability, API versioning and a project license |

The [full feature matrix](docs/features.md#planned) lists individual targets,
dependencies and minimum versions. Planned features are not available modules.

## Get Started

C++23 and CMake 3.25+; Visual Studio 2022 on Windows or GCC 14+ on Linux.
[Platform setup, optional dependencies and installation](docs/build.md).

Select only the modules your application needs:

```cmake
set(WEAVE_MODULES "tcp;runtime" CACHE STRING "" FORCE)
add_subdirectory(external/weave)

target_link_libraries(my_app PRIVATE weave::tcp weave::runtime)
```

Each module exports `weave::<name>`. Installed packages also support
`find_package(weave CONFIG REQUIRED COMPONENTS tcp)`.
[Headers and CMake targets](docs/build.md#include-only-what-you-need).

| Start With | Examples |
| --- | --- |
| TCP server | [Concurrent echo servers](modules/tcp/examples/echo/README.md) |
| TLS server | [TLS echo](modules/tls/examples/echo/README.md) |
| PostgreSQL | [Struct queries](modules/postgres/examples/queries/simple_struct.cpp), [pooling](modules/postgres/examples/connections/pool.cpp) |
| MySQL | [Queries, streaming, pools and migrations](modules/mysql/examples/README.md) |
| Data conversion | [JSON structs](modules/json/examples/struct.cpp), [database mapping](modules/database/examples/mapping.cpp) |

## Repository Map

| Location | Contents |
| --- | --- |
| [modules/](modules/) | Feature-owned public headers, implementation, guides, tests, examples and benchmarks |
| [development/](development/README.md) | Shared build, packaging, testing and benchmark infrastructure |
| [docs/](docs/README.md) | Documentation index, feature matrix, build guide and architecture |
| [.github/workflows/](.github/workflows/) | Windows correctness CI |

For changes, start with the [architecture](docs/architecture.md) and
[development rules](AGENTS.md). Each feature lives under `modules/<name>/`;
the [documentation index](docs/README.md) is the complete guide directory.

## Testing And Benchmarks

Windows CI runs Debug, Release and AddressSanitizer correctness checks.
Linux testing is available locally; Linux CI is planned. Benchmarks are manual,
hardware-specific measurements, separate from correctness gates.

[Testing and deployment limits](development/testing/README.md) |
[Benchmark guide](development/benchmarks/README.md) |
[Runtime scaling](modules/runtime/benchmarks/README.md) |
[PostgreSQL benchmarks](modules/postgres/benchmarks/README.md)

## Local Runtime Benchmarks

Hosted CI runs correctness tests only. Local measurements reserve fixed separate
client cores and test 1/2/4/8/16/32 server cores where hardware permits.

<details>
<summary>Local runtime results and provenance</summary>

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

[Full measurements and raw evidence](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261006-182604) / [methodology](modules/runtime/benchmarks/README.md).
<!-- benchmark-results:end -->

Shared IOCP stays opt-in; default-layout throughput was essentially unchanged.
[Scaling, CPU/tail findings and reproduction](modules/runtime/benchmarks/README.md) /
[latest PostgreSQL/libpq measurements](modules/postgres/benchmarks/README.md).

</details>

## License

No project license has been selected. Third-party dependencies retain their licenses.
