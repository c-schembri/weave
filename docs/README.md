# Documentation

Start with the [project README](../README.md), then open the guide for the feature you need.

| Guide | Contents |
| --- | --- |
| [Features and versions](features.md) | Implemented/planned feature tables, minimum versions and dependencies |
| [Build](build.md) | Requirements, CMake components, installation and Windows/Linux setup |
| [JSON](../modules/json/README.md) | Glaze struct conversion, buffer APIs, diagnostics and validation boundaries |
| [Execution](../modules/io/README.md) | Context, Runtime, Tasks, ownership, cancellation and timers |
| [Networking](../modules/tcp/README.md) | TCP, addresses, DNS, local sockets and stream helpers |
| [Synchronization](../modules/sync/README.md) | Bounded channels and semaphores |
| [TLS](../modules/tls/README.md) | Verified connections, mTLS, sessions, revocation and credential policy |
| [PostgreSQL](../modules/postgres/README.md) | Queries, results, streaming, COPY, pooling and blocking use |
| [MySQL](../modules/mysql/README.md) | Prepared/text queries, streaming, pooling, blocking use and migrations |
| [Migrations](../modules/database/docs/migrations.md) | Schema DSL, per-file CMake registration, execution and recovery |
| [PostgreSQL connections](../modules/postgres/docs/connections.md) | Configuration, authentication, TLS modes, failover and cancellation |
| [PostgreSQL advanced](../modules/postgres/docs/advanced.md) | Pipelines, portals, mixed exchanges, replication and large objects |
| [PostgreSQL observability](../modules/postgres/docs/observability.md) | Diagnostics, metadata, callbacks, tracing and result storage |
| [libpq mapping](../modules/postgres/docs/api-audit.md) | Function-by-function compatibility and intentional differences |
| [Benchmarks](../development/benchmarks/README.md) | Running and interpreting local comparisons |
| [Runtime scaling](../modules/runtime/benchmarks/README.md) | Weave/Asio/Tokio methodology and measured limits |
| [PostgreSQL benchmarks](../modules/postgres/benchmarks/README.md) | Matched libpq results and reproduction |
| [Testing](../development/testing/README.md) | Correctness gates, sanitizer coverage and deployment limits |
| [Architecture](architecture.md) | Module layout and implementation boundaries |

Examples: [TCP echo](../modules/tcp/examples/echo/README.md),
[TLS echo](../modules/tls/examples/echo/README.md),
[database columns](../modules/database/examples/columns.cpp),
[database mapping](../modules/database/examples/mapping.cpp),
[PostgreSQL simple struct](../modules/postgres/examples/queries/simple_struct.cpp),
[PostgreSQL column aliases](../modules/postgres/examples/queries/shared_mapping.cpp),
[PostgreSQL pooling](../modules/postgres/examples/connections/pool.cpp),
[MySQL usage](../modules/mysql/examples/README.md).

Implementation diaries and superseded qualification notes remain in Git history,
not in the user guide. Current guides describe supported contracts; tests are not
an independent security audit or a promise of drop-in libpq compatibility.
