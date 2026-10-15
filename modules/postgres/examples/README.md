# PostgreSQL Examples

Start with [simple_struct.cpp](queries/simple_struct.cpp): connect, execute,
map each row with `row.as<Shipment>()`, then shut down. It uses the same struct
and SQL as the [MySQL query example](../../mysql/examples/query.cpp).

| Source / Target | Shows |
| --- | --- |
| [queries/simple_struct.cpp](queries/simple_struct.cpp) / `postgres_simple_struct` | Automatic struct mapping, without registration |
| [queries/primitives.cpp](queries/primitives.cpp) / `postgres_primitives` | Individual fields, views and NULL |
| [queries/borrowed.cpp](queries/borrowed.cpp) / `postgres_borrowed` | Borrowed parameters, prepared statements, outcomes, binary values and caller-buffer COPY |
| [queries/struct.cpp](queries/struct.cpp) / `postgres_struct` | Cached column binding and text/binary results |
| [queries/shared_mapping.cpp](queries/shared_mapping.cpp) / `postgres_shared_mapping` | Explicit column aliases |
| [connections/pool.cpp](connections/pool.cpp) / `postgres_pool` | Scoped checkout and automatic return |
| [migrations/main.cpp](migrations/main.cpp) / `postgres_migrations` | Per-file migration registration |

```sh
cmake -S . -B build/postgres-examples -DWEAVE_MODULES=postgres -DWEAVE_BUILD_EXAMPLES=ON
cmake --build build/postgres-examples --config Release --parallel 4
```

Supply OpenSSL and ICU installations if CMake cannot discover them.
[Build requirements](../../../docs/build.md). Executables are under
`build/postgres-examples/Release/` on Windows, or directly under
`build/postgres-examples/` with a single-configuration generator.

Pass an optional connection string, or use the configuration sources loaded by
`Options::load`:

```sh
./build/postgres-examples/postgres_simple_struct "host=localhost user=app dbname=shipping"
```

Queries use constants; no application tables are needed except for migrations.
Run migrations only against a database intended for that example. TLS verification
stays enabled by default; configure trust for a private CA.
[Configuration and authentication](../docs/connections.md) |
[Detailed APIs and lifetime contracts](../docs/usage.md) |
[Pipelines, portals and replication](../docs/advanced.md).
