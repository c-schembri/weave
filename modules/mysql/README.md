# MySQL

Native MySQL classic wire-protocol client. Include `<weave/mysql.hpp>` and link
`weave::mysql`; no libmysqlclient or Runtime dependency.

## Struct Queries

```cpp
#include <weave/mysql.hpp>
#include <weave/log.hpp>
#include <string>
#include <utility>

struct Shipment {
  weave::i64 id;
  std::string recipient;
};

weave::Task<void> example(weave::mysql::Options options)
{
  auto db = co_await weave::mysql::connect(std::move(options));
  auto result = co_await db.execute("SELECT 42 AS id, 'Ada' AS recipient");

  for (const auto &row : result.rows) {
    auto shipment = row.as<Shipment>();
    if (!shipment)
      co_await weave::fail(shipment.error());

    WEAVE_LOG_INFO("Shipment %lld for %s", static_cast<long long>(shipment->id), shipment->recipient.c_str());
  }

  co_await db.disconnect();
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
| [Struct query](examples/query.cpp) | Connect, execute, map rows and disconnect |
| [Parameters](examples/prepared.cpp) | Reusable statements and borrowed inputs |
| [Primitive values](examples/primitives.cpp) | Individual fields, views and NULL |
| [Streaming](examples/stream.cpp) | Incremental rows and explicit draining |
| [Pooling](examples/pool.cpp) | Scoped checkout and automatic return |
| [Blocking](examples/blocking.cpp) | Synchronous calls |
| [Batch outcomes](examples/batch.cpp) | Ordered commands and SQL errors |
| [TLS](examples/tls.cpp) | Private CA configuration |
| [Migrations](examples/migrations/main.cpp) | Per-file schema changes |

## Contracts

TLS verifies certificates and hostnames by default. Use one connection per
concurrent operation, or a pool. Drain outstanding Tasks and handles before
destroying a connection; `disconnect()` performs graceful shutdown.

Experimental, not production-certified or a full native C API replacement.
[Detailed APIs, security, lifetimes and limits](docs/usage.md) |
[Qualification and benchmarks](docs/usage.md#qualification-and-benchmarks) |
[Migration guide](../database/docs/migrations.md).
