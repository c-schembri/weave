# MySQL Examples

Each source is a complete program; link `weave::mysql`. No native MySQL client SDK
or Runtime is required. Queries use constants, so no application tables are needed.

| Source / Target | Shows |
| --- | --- |
| [query.cpp](query.cpp) / `mysql_query` | Automatic struct mapping, without registration |
| [primitives.cpp](primitives.cpp) / `mysql_primitives` | Named columns, integers, doubles, booleans, views and NULL |
| [prepared.cpp](prepared.cpp) / `mysql_prepared` | Reusable statement and borrowed stack-array parameters |
| [stream.cpp](stream.cpp) / `mysql_stream` | Incremental owning rows and explicit draining |
| [batch.cpp](batch.cpp) / `mysql_batch` | Ordered SQL outcomes, including an intentional SQL failure |
| [pool.cpp](pool.cpp) / `mysql_pool` | Scoped checkout and automatic return |
| [blocking.cpp](blocking.cpp) / `mysql_blocking` | Synchronous checked results on the calling thread |
| [tls.cpp](tls.cpp) / `mysql_tls` | Explicit private CA and configurable port |
| [migrations/main.cpp](migrations/main.cpp) / `mysql_migrations` | Per-file migration registration and explicit nontransactional DDL |

```sh
cmake -S . -B build/mysql-examples -DWEAVE_MODULES=mysql -DWEAVE_BUILD_EXAMPLES=ON
cmake --build build/mysql-examples --config Release --parallel 4
```

Supply an OpenSSL 3.5+ installation if CMake cannot discover one. Executables are
under `build/mysql-examples/modules/mysql/examples/` (inside `Release/` on Windows).

Examples use `weave` at `127.0.0.1:3306`; pass an optional port argument and set
`MYSQL_PASSWORD` in the process environment. Missing means an empty password.
Standard examples use default trust (including OpenSSL's `SSL_CERT_FILE` override);
the certificate must verify for `127.0.0.1`. For a private CA or another port:

```sh
./build/mysql-examples/modules/mysql/examples/mysql_tls /path/to/ca.pem 3306
```

The migration example additionally requires a `shipping` database and creates its
`shipments` table/history. Run it only against a database intended for this example.
No example disables TLS verification. For mTLS add `certificate_file` and
`private_key_file` to the client TLS policy. Keep borrowed arrays/views alive until
their Tasks and cancellation cleanup finish; statement/stream handles lease their
connection. [API, security and lifetime contracts](../README.md).
