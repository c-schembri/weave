# Documentation

Start with the [project README](../README.md), then open the guide for the feature you need.

| Guide | Contents |
| --- | --- |
| [Build](build.md) | Requirements, CMake components, installation and Windows/Linux setup |
| [Execution](execution.md) | Context, Runtime, Tasks, ownership, cancellation and timers |
| [Networking](networking.md) | TCP, addresses, DNS, local sockets and stream helpers |
| [Synchronization](synchronization.md) | Bounded channels and semaphores |
| [TLS](tls.md) | Verified connections, mTLS, sessions, revocation and credential policy |
| [PostgreSQL](postgres.md) | Queries, results, streaming, COPY, pooling and blocking use |
| [PostgreSQL connections](postgres-connections.md) | Configuration, authentication, TLS modes, failover and cancellation |
| [PostgreSQL advanced](postgres-advanced.md) | Pipelines, portals, mixed exchanges, replication and large objects |
| [PostgreSQL observability](postgres-observability.md) | Diagnostics, metadata, callbacks, tracing and result storage |
| [libpq mapping](postgres-api-audit.md) | Function-by-function compatibility and intentional differences |
| [Benchmarks](benchmarks.md) | Running and interpreting local comparisons |
| [Runtime scaling](runtime-scaling.md) | Weave/Asio/Tokio methodology and measured limits |
| [PostgreSQL benchmarks](postgres-benchmarks.md) | Matched libpq results and reproduction |
| [Testing](testing.md) | Correctness gates, sanitizer coverage and deployment limits |
| [Architecture](architecture.md) | Module layout and implementation boundaries |

Examples: [TCP echo](../modules/tcp/examples/echo/README.md),
[TLS echo](../modules/tls/examples/echo/README.md),
[database columns](../modules/database/examples/columns.cpp),
[database mapping](../modules/database/examples/mapping.cpp),
[PostgreSQL simple struct](../modules/postgres/examples/queries/simple_struct.cpp),
[PostgreSQL column aliases](../modules/postgres/examples/queries/shared_mapping.cpp),
[PostgreSQL pooling](../modules/postgres/examples/connections/pool.cpp).

Implementation diaries and superseded qualification notes remain in Git history,
not in the user guide. Current guides describe supported contracts; tests are not
an independent security audit or a promise of drop-in libpq compatibility.
