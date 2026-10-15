# PostgreSQL

Weave implements PostgreSQL's wire protocol directly; it does not wrap or link
libpq. Include `<weave/postgres.hpp>` and link `weave::postgres`.
OpenSSL Crypto and ICU implement authentication primitives and SASLprep.
Runtime is optional.

Version schema changes with the shared [migration DSL and CMake manifest helper](../database/docs/migrations.md).

The module is experimental, not drop-in libpq or independently security audited.
See the [API mapping](docs/api-audit.md), [validation boundaries](../../development/testing/README.md)
and [matched libpq measurements](benchmarks/README.md).

## Queries

```cpp
#include <weave/postgres.hpp>

weave::Task<void> example()
{
  auto database = co_await weave::pg::connect({
    .host = "localhost",
    .user = "app",
    .database = "app",
    .password = "secret",
  });

  std::vector<weave::pg::Parameter> parameters{{"42", 23}};
  auto result = co_await database.execute("SELECT $1::int", parameters);

  auto number = result.rows.front().get<int>(0);
  if (!number)
    co_await weave::fail(number.error());

  co_await database.finish();
}
```

Run this Task with [Context or Runtime](../io/README.md). TLS verifies the server's
chain and DNS/IP identity by default. Supply `Options::tls` for private CAs or
mTLS; weaker modes require explicit opt-in.
[Connection configuration and authentication](docs/connections.md).

| API | Purpose |
| --- | --- |
| `query` | Simple SQL; all result sets, including multiple statements |
| `execute` | Extended query with owned text, binary or NULL parameters |
| `prepare`, `execute_prepared`, `describe`, `close_prepared` | Named prepared statements |
| `query_outcomes`, `execute_outcome`, `execute_prepared_outcome` | Owning successes and SQL diagnostics without automatic SQL-error propagation |
| `call_function` | Legacy FunctionCall by server-resolved OID; parameter types must be zero |
| `batch`, `pipeline` | Correlated commands and explicit synchronization barriers |

Default query methods propagate SQL failures through `Task`. Outcome methods
return SQL failures as owning values; transport, cancellation, resource and
protocol errors still fail the Task. A simple query stops at its first SQL
error; unexecuted statements do not get fabricated results.

Results own rows, column OIDs/formats and command tags. Fields support checked
`as<T>()`, `row.get<T>(index/name)`, primitive helpers and independent sequential
readers in either text or binary mode. `Value::integer<T>()` and
`binary_integer<T>()` remain explicit low-level wire-format decoders.
NULL is distinct from an empty value. Results survive later queries and
connection destruction. [Results, formatting and storage](docs/observability.md#results).

```cpp
const auto &row = result.rows.front(); // Check for an empty result first.
auto weight = row["weight"].as<double>();
auto delivered = row.get<bool>("delivered");
auto notes = row["notes"].optional<std::string_view>();

auto fields = row.reader();
auto id = fields.next<weave::i64>();
auto quantity = fields.next().integer<int>();
```

Every conversion returns a synchronous `Result<T>`. Check it before use; inside
a Task, propagate failure with `co_await weave::fail(value.error())`. See the
[complete primitive example](examples/queries/primitives.cpp)
(`postgres_primitives`) and [conversion contracts](docs/observability.md#primitive-access).

For structs, member names select columns automatically; no registration is needed:

```cpp
struct Shipment {
  weave::i64 id;
  double weight;
  std::string recipient;
};

auto shipment = row.as<Shipment>(); // Result<Shipment>
```

Use `result.decoder<Shipment>()` to bind column indices once for repeated rows.
Start with the [simple struct example](examples/queries/simple_struct.cpp)
(`postgres_simple_struct`): `row.as<Shipment>()`, with no registration.
The [cached decoder example](examples/queries/struct.cpp)
(`postgres_struct`) adds reusable column binding and text/binary results.
See [struct conversion contracts](docs/observability.md#struct-access).
For optional column aliases, see the [shared mapping example](examples/queries/shared_mapping.cpp)
(`postgres_shared_mapping`), using `db::RowTraits`.

## Borrowed Inputs

See the complete [runnable example](examples/queries/borrowed.cpp)
(`postgres_borrowed`, built with `WEAVE_BUILD_EXAMPLES=ON`). Pass a connection
string as its optional argument, or use the sources loaded by `Options::load`.
TLS verification stays enabled by default.

The owning methods retain SQL and parameters in their lazy Tasks. For hot paths,
`execute_borrowed` accepts SQL as `string_view` and an allocation-free parameter
span. C arrays, `std::array` and existing vectors all work:

```cpp
const std::array parameters{
  weave::pg::ParameterView{.data = "42", .type = 23}
};

auto result = co_await database.execute_borrowed("SELECT $1::int", parameters);
```

`ParameterView` holds optional `string_view` data; disengaged means NULL,
not an empty value. Binary parameters can contain NUL bytes with `Format::binary`.
`prepare_borrowed` takes name/SQL views and a span of type IDs;
`execute_prepared_borrowed`, `execute_outcome_borrowed` and
`execute_prepared_outcome_borrowed` use the same input lifetime contract.
BlockingConnection exposes the same methods as synchronous Results.

Keep SQL, names, parameter arrays and the data they reference alive until the
Task completes, including cancellation cleanup. Do not pass temporary containers
or mutate borrowed inputs while the operation executes. Deferred Tasks already
retain the Connection's borrow, but do not own these input buffers. Pipeline
queueing and batch methods remain owning because submission and sending are separate.

These paths avoid input-container/string copies and the temporary type-ID vector,
not all heap allocation: protocol encoding, owning results and coroutine frames
still use the existing allocation model. An array in a coroutine may itself live
inside a heap-allocated frame.

## Pooling

See the [runnable pool example](examples/connections/pool.cpp)
(`postgres_pool`): four concurrent requests share two sessions on one Context.

```cpp
weave::Task<void> example(weave::pg::Options options)
{
  auto pool = co_await weave::pg::Pool::create(
    std::move(options),
    {.max_connections = 16, .min_connections = 2});

  {
    auto database = co_await pool.acquire();
    auto result = co_await database->execute("SELECT 42");
  } // Returns the lease automatically.

  pool.close();
}
```

A move-only `PooledConnection` exposes the ordinary Connection through
`->` / `*`. Destruction or synchronous `release()` returns it;
`discard()` retires it. Drain every operation, pipeline and streaming borrow
before releasing a lease. Moving the underlying Connection out is a fatal
contract violation, and its `reset()` returns `operation_not_supported`.
Use another pool to change configuration.

Borrowed Connection references, prepared names, large-object descriptors and
CancelHandles belong to that checkout. Do not use them after releasing its lease;
a retained backend CancelHandle could otherwise cancel a later borrower's work.

| PoolOptions | Default and policy |
| --- | --- |
| `max_connections` | 16; accepted range 1-65,536 |
| `min_connections` | 1; initial connections, at most the maximum; zero enables lazy startup |
| `acquire_timeout` | 30 seconds; positive budget covers waiting, connection establishment and cleanup |
| `idle_timeout` | 5 minutes; zero disables idle retirement |
| `max_lifetime` | 30 minutes; zero disables lifetime retirement |

Capacity admission is bounded and FIFO. Idle selection is LIFO, with expiration
checked on checkout rather than by a background thread. Initial connection
creation uses the connection's own startup deadlines; wrap `Pool::create` in
`timeout` if the complete prewarm needs a deadline.

Before reuse, the pool rolls back an open/failed transaction and executes
`DISCARD ALL`. This removes session settings, temporary tables, prepared
statements, LISTEN registrations and session advisory locks. Application
callbacks, pending notices/notifications and local result state are cleared
when a lease returns. Prepared statements cannot be assumed to survive checkout.
Unfinished COPY/streaming or closed sessions are retired.

A cleanup failure retires that session and fails the acquisition; a subsequent
acquisition may connect afresh. No query is automatically retried or replayed.
Replication startup is unsupported in pools. Owned connection credentials are
retained for replacement sessions and cleansed when the last state owner ends;
caller copies remain the application's responsibility.

The pool belongs to its creating Context or compatible work-stealing domain.
A worker-affine Runtime needs one pool per worker Context. Do not share a pool
across unrelated Contexts, even on one thread. The Context must outlive the pool,
leases and deferred acquisitions; keep Runtime-bound pools inside that domain.

`close()` is synchronous: reject/wake acquisitions and retire idle sessions.
Existing borrowers finish independently and are discarded on return. It does
not cancel or join borrowers. Destruction closes the pool; outstanding leases
retain its state. `stats()` snapshots idle, borrowed, pending, created, discarded
and closed counters. Pending includes executing acquisitions, not unstarted Tasks.

## Streaming And COPY

`for_each_row` awaits a row callback for each row without retaining the full
result. Callback failure retires the session. For explicit consumption,
`start_rows` / `read_row` expose incremental rows; `row_columns()` borrows
current metadata. Finish the stream before another command.

`start_copy`, `write_copy`, `read_copy` and `end_copy` handle COPY
phases. COPY output EOF is not returned until final command/ReadyForQuery
processing completes. `copy_result()` exposes the terminal result.
Keep borrowed buffers alive through the await; returned owning chunks can be
retained. Do not issue unrelated commands during COPY.

For reusable caller-owned storage:

```cpp
std::array<std::byte, 8192> buffer;

while (auto received = co_await database.read_copy(buffer)) {
  auto bytes = std::span{buffer}.first(received->size);
  // Consume bytes before the buffer is reused.
  // received->end_of_message distinguishes complete CopyData messages.
}
```

`read_copy(span<byte>)` returns `optional<CopyRead>`: nullopt is EOF, while an
engaged zero-size value is an empty CopyData message. The buffer must be nonempty
and survive Task completion/cancellation cleanup. Large messages are delivered
in pieces without truncation or a full-payload vector allocation. The next read
continues the same message until `end_of_message` is true. Between pieces, the
owning `read_copy()` and trace replacement return `busy`; finish the message
before switching APIs. COPY BOTH `end_copy()` can drain an unfinished piece.
Connection-wide message limits still apply. BlockingConnection has the same
buffer overload.

Disabled/metadata tracing does not retain COPY payloads. Explicit application
tracing retains only its configured prefix and emits one trace at the complete
message boundary, not one per piece. Protocol metadata, enabled tracing and
coroutine machinery may still allocate; this is not an allocation-free connection.

For bounded owning query chunks, use `exchange(sql, {.chunk_rows = 256})`.
For portals, pipelines, mixed SQL/COPY, replication and large objects, see
[advanced operations](docs/advanced.md).

## Encoding And Passwords

Parameters/results are bytes, not automatically transcoded strings.
`client_encoding()` is synchronous; `set_client_encoding(Encoding)` changes
the server session asynchronously and validates its reported setting.

Prefer parameters. Where SQL construction requires quoting, synchronous
`escape_literal` / `escape_identifier` return fully quoted strings, reject
NUL/invalid encoding and respect multibyte boundaries. Keep the encoding
unchanged until execution. `encode_bytea` / `decode_bytea` handle bounded
owning binary-text conversion, not SQL quoting.
[Encoding and display contracts](docs/observability.md#encoding).

`change_password` generates a verifier rather than placing the plaintext
password in SQL. [Verifier policy](docs/connections.md#passwords).

## Ownership And Cancellation

A Connection is a serialized session, not a thread-safe query dispatcher.
Deferred Tasks capture its borrow immediately and block reset/conflicting leases
even before execution. Own or join those Tasks before destruction.
Use one connection per concurrent operation, or a pool.

Local Task cancellation of an active exchange retires the session after native
operations drain. PostgreSQL CancelRequest is a separate API: capture a
`CancelHandle`, await its `request()`, then await/drain the original command.
It is not proof of rollback or of whether a command committed.
[Cancellation and reset](docs/connections.md#cancellation-and-reset).

`close()` is immediate local teardown once operations have drained;
`finish()` sends normal protocol termination asynchronously. Inspect
`last_failure()` only after operations/leases drain.
[Diagnostics and callbacks](docs/observability.md).

## Blocking Use

`BlockingConnection` returns synchronous `Result<T>` from the same protocol
engine, driven by its private caller-thread Context. It does not add a hidden
runtime or networking thread. Use it outside an executing Context; recursive
event-loop pumping is forbidden.

The pool's Task API can also be driven synchronously by `Context::run`, on
that Context's owner thread. There is no separate BlockingPool.
