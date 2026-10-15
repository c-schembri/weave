# PostgreSQL

Native PostgreSQL wire-protocol client. Include `<weave/postgres.hpp>` and link
`weave::postgres`; no libpq or Runtime dependency.

## Struct Queries

```cpp
#include <weave/postgres.hpp>
#include <weave/log.hpp>
#include <string>
#include <utility>

struct Shipment {
  weave::i64 id;
  std::string recipient;
};

weave::Task<void> example(weave::pg::Options options)
{
  auto db = co_await weave::pg::connect(std::move(options));
  auto result = co_await db.execute("SELECT 42 AS id, 'Ada' AS recipient");

  for (const auto &row : result.rows) {
    auto shipment = row.as<Shipment>();
    if (!shipment)
      co_await weave::fail(shipment.error());

    WEAVE_LOG_INFO("Shipment %lld for %s", static_cast<long long>(shipment->id), shipment->recipient.c_str());
  }

  co_await db.finish();
}
```

Member names select matching columns automatically; no registration is needed.
`std::string` members own their values. Decoding returns a synchronous
`Result<Shipment>`; query failures propagate through the Task.

Pass connection settings in `Options` and run the Task with
[Context or Runtime](../io/README.md). The query needs no application tables.
[Complete program and connection setup](examples/README.md).

## More Examples

| Example | Shows |
| --- | --- |
| [Struct query](examples/queries/simple_struct.cpp) | Connect, execute, map rows and disconnect |
| [Parameters](examples/queries/borrowed.cpp) | Prepared statements, borrowed inputs and caller-buffer COPY |
| [Primitive values](examples/queries/primitives.cpp) | Individual fields, views and NULL |
| [Cached mapping](examples/queries/struct.cpp) | Reusable column binding and text/binary results |
| [Column aliases](examples/queries/shared_mapping.cpp) | Explicit mappings when member and column names differ |
| [Pooling](examples/connections/pool.cpp) | Scoped checkout and automatic return |
| [Migrations](examples/migrations/main.cpp) | Per-file schema changes |

## Contracts

TLS verifies certificates and hostnames by default. Use one connection per
concurrent operation, or a pool. Drain outstanding Tasks and handles before
destroying a connection; `finish()` performs graceful shutdown.

Experimental, not production-certified, independently security audited or a
drop-in libpq replacement. OpenSSL Crypto and ICU provide authentication primitives.
[Detailed APIs and lifetimes](docs/usage.md) |
[Configuration and authentication](docs/connections.md) |
[Streaming, pipelines and COPY](docs/usage.md#streaming-and-copy) |
[Results and diagnostics](docs/observability.md) |
[API mapping](docs/api-audit.md) |
[Qualification](../../development/testing/README.md) |
[Benchmarks](benchmarks/README.md).
