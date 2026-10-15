# MySQL

Include `<weave/mysql.hpp>` and link `weave::mysql`. This classic wire-protocol client
uses Weave TCP/TLS and shared Database storage/mapping, not libmysqlclient. Runtime
is optional; PostgreSQL, ICU and JSON are not dependencies.

[Complete examples](examples/README.md): struct mapping, primitive
types/NULL, prepared parameters, streaming, batch errors, pooling, blocking calls,
private-CA TLS and migrations. No application tables are needed except for migrations.

## Queries And Parameters

Inside a Task driven by a Context or Runtime:

```cpp
auto db = co_await weave::mysql::connect({
  .user = "app", .password = "secret", .database = "shipping"});

auto statement = co_await db.prepare("SELECT id, recipient FROM shipments WHERE id = ?");
const std::array<weave::mysql::ParameterView, 1> parameters{42};
auto result = co_await statement.execute_borrowed(parameters);
for (const auto &row : result.rows) {
  auto recipient = row["recipient"].as<std::string_view>();
  if (!recipient)
    co_await weave::fail(recipient.error());
  // Use *recipient while row remains alive and unchanged.
}
co_await statement.close();
co_await db.disconnect();
```

| API | Behavior |
| --- | --- |
| `connect(Options)` | Own configuration immediately; lazily connect/authenticate on active execution |
| `execute(sql)` | Own SQL; return one text result |
| `execute(sql, parameters)` | Prepare/bind/execute/close a one-shot binary statement |
| `prepare(sql)` | Return a reusable server Statement with parameter/column metadata |
| `Statement::execute(parameters)` | Own parameters; return one binary result |
| `query(...)` | Return all results in wire order, including stored-procedure responses |
| `stream(...)` | Return a session-leasing RowStream without buffering all rows |
| `batch(commands)` | Own/run ordered commands sequentially; retain SQL failures as outcomes |
| `reset()` | Reset server state and restore configured charset/database |
| `close()` | Synchronously abort an idle session |
| `disconnect()` | Send QUIT and TLS close_notify; no server acknowledgement |

Owned SQL/parameters copy lvalues and transfer rvalues. `_borrowed` overloads take
`string_view`/`span` without input-container copies: retain the array, SQL and
referenced bytes unchanged through Task completion **and cancellation cleanup**.
Literals/`const char *` work; binding uses `?`, never SQL interpolation. With GCC 15,
use named arrays/vectors instead of nested initializer-list temporaries directly
across `co_await` (a compiler coroutine-lowering bug).

`Parameter` owns text/blob/decimal bytes; `ParameterView` borrows them. Scalars,
Date, DateTime and Time are inline. Default/nullopt means NULL, not empty bytes.
Both support bool, signed/unsigned integers, float/double, strings, byte spans,
calendar components and exact decimal wrappers. Invalid values are rejected.
`send_long_data(index, bytes)` sends string/blob fragments; execute with an empty
string/blob binding at that index. Completion acknowledges transport, not server
acceptance. Execute/reset clears fragments; no hidden retries.

Semicolon SQL requires `Options::multi_statements = true`. Stored procedures may
return multiple results without that opt-in. Single-result execute drains extras
before returning `ClientError::multiple_results`; use query/stream to retain them.
Batch is sequential, not pipelining, and adds no transaction. Ordered
`Outcome {results, failure}` values retain SQL failures; transport/protocol failures
still fail the Task.

## Rows And Streaming

ResultSet owns columns/rows, affected-row and insert-ID counts, warnings/status and
server info. Rows retain immutable schema after result/session destruction. Field
selectors accept index or exact case-sensitive name; duplicates are errors.

`Field::as<T>()` checks range/width/format/NULL. Supported types are integers,
float/double, bool (only 0/1), strings/views, Date/DateTime/Time, Decimal/DecimalView
and their optional forms. Calendar decoding preserves zero dates and microseconds
without timezone conversion; Time may be negative or longer than a day. Decimal
preserves exact text. `bytes()` exposes raw payloads; BIT/GEOMETRY are not decimal
integers. Binary fixed-width numeric/calendar values are not strings; float-to-int
coercion is not implicit. Fields/views borrow an unchanged Row; strings own copies.

Automatic struct mapping uses shared Database reflection without registration:

```cpp
struct Shipment {
  weave::i64 id;
  std::string recipient;
};

for (const auto &row : result.rows) {
  auto shipment = row.as<Shipment>();
  if (!shipment)
    co_await weave::fail(shipment.error());
}
```

Use a non-local simple aggregate. Optional `weave::db::RowTraits<T>` supplies aliases;
view members still borrow their Row. Public column edits do not mutate retained schemas.

```cpp
auto stream = co_await db.stream("SELECT id FROM shipments");
while (auto row = co_await stream.next()) {
  auto id = (*row)["id"].as<weave::i64>();
  if (!id)
    co_await weave::fail(id.error());
}
co_await stream.finish();
```

`next()` returns an owning row or current-result EOF. After EOF, `next_result()`
advances metadata; `summary()` synchronously copies current metadata/counts, not rows.
`finish()` drains every remaining result. Abandoning an undrained stream retires the
session, not a hidden blocking drain. No eager row decoding or server cursor/fetch;
bounded transport staging can read ahead across packet boundaries. Deferred
next/next_result Tasks capture their result generation, never silently retargeting.

## Blocking And Pooling

```cpp
auto db = weave::mysql::BlockingConnection::connect(options);
if (!db)
  return weave::report_error(db.error());
auto result = db->execute("SELECT 42 AS id");
if (!result)
  return weave::report_error(result.error());
```

BlockingConnection drives a private Context on the caller, without a helper thread.
Result-returning statement/stream/query/batch/reset APIs mirror asynchronous use;
child wrappers retain that driver. Call outside an executing Context, on the owning
thread; no concurrent use. Destruction is not graceful disconnect.

```cpp
auto pool = co_await weave::mysql::Pool::create(options, {.max_connections = 8});
auto db = co_await pool.acquire();
auto result = co_await db->execute("SELECT 42 AS id");
```

Pool owns configuration for replacement sessions and binds to its creating Context
or work-stealing domain. Return/release each checkout only after its Tasks,
statements and streams release their leases; violations are fatal contracts.
Next acquisition resets transactions, temporary tables, user variables, statements
and server locks before reuse, restoring configured charset/database. With no
configured database, a later USE requires reconnect: reset cannot deselect it.
Capacity/acquisition/idle/lifetime policies and stats use shared Database definitions.
`discard()` retires a checkout. `close()` wakes/rejects acquisitions, not joins borrowers;
retain the transport Context until borrowers drain.

## Migrations

The shared [migration DSL and file generator](../database/docs/migrations.md) accept async or blocking
MySQL connections. Every entry must explicitly select `MigrationTransaction::none`:
MySQL DDL implicitly commits. GET_LOCK serializes cooperating runners; InnoDB history
retains durable applying/reverting markers. Failure requires manual schema/history
reconciliation, never automatic retry or fictitious atomic rollback. Use
`schema::drop_index(table, index)` for table-scoped MySQL indexes.

## Security And Failure Contracts

Certificate **and hostname** TLS verification is mandatory by default, without fallback.
For private CA/mTLS pass an owned TlsContext::client policy in Options::tls.
Unverified/certificate-only policies fail. Authentication supports caching_sha2_password
(full/fast) and mysql_native_password, including bounded plugin switches. The client
does not enable the legacy server plugin.

Explicit `plaintext=true` excludes Options::tls and allows challenge proofs, **never**
full cleartext passwords. Caching-SHA2 full auth on plaintext fails; RSA/server-key
retrieval is not implemented. Password copies share immutable, best-effort-cleansed
storage; caller copies and allocator/third-party history are not guaranteed erased.
No hidden option files or environment reads.

Connect timeout covers TCP/TLS/authentication. Packet/result/row/column/result-count/
statement limits are delivery/admission budgets, **not exact heap/RSS caps**.
Streaming bounds each delivered row/schema, not total application-retained rows.
Batch retention also charges previous responses, metadata and SQL diagnostics.

Serialize protocol use. Lazy Tasks retain session borrows before starting; moving/
destroying a borrowed Connection is fatal. Active streams own protocol use; competing
calls within a serialized graph return busy. Those checks are not a mutex: independent
roots must not concurrently access the same session. Completed server ERRs permit
reuse. `last_failure()` synchronously
copies `{number, sqlstate, message}`; a subsequent command clears it.
Transport/protocol/resource failure or cancellation after advancement retires/drains
the session. Pre-start cancellation does not advance it. Local cancellation does not
issue KILL QUERY; server work may continue until disconnect is noticed.
No implicit reconnect/transaction rollback.

Intentional exclusions: compression, LOCAL INFILE, additional auth/RSA, server cursors,
query attributes, session tracking, replication/X Protocol. This is not full native
C API parity. Borrowed input and streaming avoid input-container/all-result copies,
not all allocation.

## Qualification And Benchmarks

Windows CI runs deterministic wire/parser/compiler tests, benchmarks disabled.
Live qualification creates a temporary private instance, never a user service.
Supply matching binaries, their runtime dependencies and the OpenSSL executable:

```sh
cmake -S . -B build/mysql -DWEAVE_MODULES="mysql;runtime" -DWEAVE_BUILD_TESTS=ON \
  -DWEAVE_MYSQL_TEST_SERVER=/path/to/mysqld -DWEAVE_MYSQL_TEST_CLIENT=/path/to/mysql
cmake --build build/mysql --config Release --parallel 4
ctest --test-dir build/mysql -C Release -R "mysql|package_components_mysql" --output-on-failure --no-tests=error
```

Tests cover auth/TLS, packet boundaries, text/binary types, prepared/long-data use,
multi-results, SQL recovery, cancellation, stream abandonment, pooling/multicore,
blocking calls and migration recovery on Windows IOCP and Linux io_uring.

The [native comparison](benchmarks/compare.cpp) is an optional
SDK-dependent executable, never a CI timing test:

```sh
cmake -S . -B build/mysql -DWEAVE_MODULES="mysql;runtime" \
  -DWEAVE_MYSQL_BENCHMARKS=ON -DWEAVE_MYSQL_NATIVE_ROOT=/path/to/mysql-sdk
cmake --build build/mysql --config Release --target weave_mysql_compare
python modules/mysql/tests/live.py --server /path/to/mysqld --client /path/to/mysql \
  --test /path/to/weave_mysql_compare --test-timeout 180
```

Identical SQL/TLS and checked consumption; sequential paired drivers with alternating
order, 150 ms warmup and seven 750 ms samples. Operations mean whole workloads, not
individual rows. Four-worker use means independent Weave runtime roots versus four
native threads/connections. Throughput/p99/process CPU are reported; Windows cycles
supplement coarse timer-based CPU accounting. These are client/loopback measurements,
not server-capacity, isolated-core scaling or zero-copy comparisons.

[Protocol reference](https://dev.mysql.com/doc/dev/mysql-server/8.0.46/PAGE_PROTOCOL.html).

## Paired Comparisons

Use the [paired runner](benchmarks/paired.py) with frozen before/after
executables and the same private fixture. It preserves every sample in fixed
before/native/after rotations; diagnostic subsets are not full comparisons.

```sh
export WEAVE_MYSQL_BENCHMARK_BEFORE=/absolute/path/to/before/weave_mysql_compare
export WEAVE_MYSQL_BENCHMARK_AFTER=/absolute/path/to/after/weave_mysql_compare
export WEAVE_MYSQL_PAIRED_SETTLE_SECONDS=60
python modules/mysql/tests/live.py --server /path/to/mysqld --client /path/to/mysql \
  --test modules/mysql/benchmarks/paired.py --test-timeout 600 \
  --client-cpus 0,2,4,6 --server-cpus 1,3,5,7 > paired.csv 2> paired.log
```

Choose disjoint allowed CPU sets for the actual host; placement does not reserve
cores or establish VM host topology. Windows settling may be set to 0. Keep raw
samples, failures, CPU/tail metrics and variability in ignored output directories.
No universal native-performance parity is claimed. [Benchmark methodology](../../development/benchmarks/README.md).
