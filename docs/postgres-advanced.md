# PostgreSQL Advanced Operations

These APIs share the Connection's serialized protocol engine. Keep Connection,
execution owner and lease objects alive until every deferred/active Task drains.
Independent Runtime roots must not operate on the same session concurrently.
See [core API](postgres.md) and [execution ownership](execution.md).

## Pipelines

`Connection::pipeline()` synchronously returns `Result<Pipeline>` and reserves
the session. `batch` is the simpler one-Sync convenience operation.

```cpp
auto pipeline = database.pipeline();
if (!pipeline)
  co_await weave::fail(pipeline.error());

auto command = pipeline->execute({"SELECT $1::int", {{"42", 23}}});
if (!command)
  co_await weave::fail(command.error());

if (auto barrier = pipeline->sync(); !barrier)
  co_await weave::fail(barrier.error());

co_await pipeline->flush();

while (auto result = co_await pipeline->next()) {
  // Inspect result->id, kind, complete and owning outcome.
}

if (auto finished = pipeline->finish(); !finished)
  co_await weave::fail(finished.error());
```

Execute, prepare, execute_prepared, describe, close_prepared, describe_portal,
close_portal and Sync queue synchronously without socket I/O. Commands receive
monotonic IDs; results preserve submission order, IDs and kinds. Sync is an
explicit barrier with transaction status, not a fabricated query result.

| Driver | Contract |
| --- | --- |
| `request_flush()` | Queue a five-byte backend Flush; no Sync, ID, admission slot or result |
| `flush()` | Snapshot, write and drain responses concurrently; no implicit Sync |
| `send()` | Write snapshot plus Flush without reading responses |
| `receive()` | Read submitted snapshot into the result queue without writing |
| `next()` | Consume result queue, not socket I/O |

Send completion means transport acceptance, not server execution/commit.
A receiver started first can reserve outgoing work for a partner sender.
Later queued commands belong to later snapshots. One sender, receiver and result
consumer may overlap in the same serialized coroutine graph; flush cannot mix
with an unfinished split exchange.

### Streaming And Backpressure

Positive `RowOptions::chunk_rows` enables owning partial chunks. Zero buffers
the command. Chunks have `complete=false`; final completion has `complete=true`,
zero rows and the command tag. Retained-data pressure may emit smaller chunks.
The bounded delivery queue needs a consumer progressing alongside transport;
awaiting a large streaming flush without consumption can wait indefinitely.

```cpp
co_await weave::scope([&](weave::TaskScope &tasks) -> weave::Task<void> {
  auto transport = tasks.spawn(weave::when_all(pipeline.send(), pipeline.receive()));
  if (!transport)
    co_await weave::fail(transport.error());

  co_await consume(pipeline);
});
```

Keep Pipeline/Connection outside the scope body. `when_all` serializes the two
driver children even on a work-stealing Runtime; do not spawn separate stealable
roots on one Connection. Scope cleanup cancels/drains transport if the consumer
fails. `when_all` alone is not fail-fast cleanup for a backpressured producer.
Cancellation of `next()` alone does not cancel the transport driver.

SQL failures are owning outcomes; later commands are aborted until a valid Sync.
An earlier Sync cannot clear later synchronization debt. Already delivered rows
are provisional until final command/transaction outcome; errors cannot retract
application effects. Success is not necessarily commit. No replay/retry occurs.

`finish()` synchronously releases ownership only after writes, responses, Sync
and method Tasks drain. An unsent Flush prevents finish; dropping wholly unsent
work sends nothing. Dropping unfinished work after I/O retires the connection.

## Portals

```cpp
co_await database.query("BEGIN");
co_await database.open_portal("items", "SELECT id FROM items ORDER BY id");

auto metadata = co_await database.describe_portal("items");
auto page = co_await database.fetch("items", 64);

co_await database.close_portal("items");
co_await database.query("COMMIT");
```

`fetch` bounds execution; `suspended` indicates more rows. Complete/close ordinary
portals before commit. Description returns owning metadata without execution,
rows or a command tag. Empty name refers to the unnamed portal, which later
extended/simple queries may replace or destroy. SQL-created cursor hold/format
behavior remains PostgreSQL's; decode formats on the actual fetch result.
SQL errors drain normally; failed transactions need rollback.

## Mixed Exchanges

`exchange(sql, RowOptions={})` returns an owning session lease whose `next()`
yields these owning events in wire order:

| Event | Meaning |
| --- | --- |
| `ResultSet` | Rows, partial chunk or command completion |
| `CopyFormat` | Input, output or bidirectional COPY phase |
| `std::vector<std::byte>` | Incoming raw CopyData |
| `CopyDone` | Receive direction ended, not entire query |
| Empty optional | ReadyForQuery consumed; exchange ended |

Positive chunk_rows streams ordinary query rows with copied schema, followed by
a zero-row terminal result. Hold returned chunks only within application memory
budgets. SQL failure drops undelivered partial rows; delivered chunks remain
provisional. Transport/protocol/active cancellation failures retire the session.

In input/bidirectional phases, `write(span)` sends data and `finish_send()` sends
CopyDone. An error string sends CopyFail for input COPY, not COPY BOTH.
Server CopyDone never implicitly closes application sends. Deferred writers
capture phase generation and cannot operate in a later COPY phase.
Continue through terminal results and ReadyForQuery, then call synchronous
`finish()`. It requires no method Tasks; dropping unfinished exchanges retires
the session. Ordinary SQL errors drain before propagation.

## Replication

Startup explicitly selects physical or database replication. Physical mode
requires TargetSession::any; SQL-based classification is inappropriate there.
Both modes require simple query protocol: extended execution, portals, FunctionCall,
batches and pipelines fail locally as unsupported. The server enforces privileges.

Use query for IDENTIFY_SYSTEM and start_copy/exchange for START_REPLICATION or
BASE_BACKUP. Payloads are opaque, not automatically interpreted as rows or WAL.
COPY BOTH permits coordinated reading/writing in one serialized graph.
`finish_copy_send()` ends only sending; server CopyDone ends only receiving.
Join readers before `end_copy()`, which discards remaining data and drains
terminal results. `copy_results()` retains multiple results; replication metadata
may have an empty command tag. Only structurally valid control keepalive tails
are tolerated after CopyDone, without automatic acknowledgment.

This is transport, not a replica: callers manage slots, decode plugins, persist
WAL, schedule feedback and acknowledge actual durable/applicable progress.
No automatic timeline following, archive restore, pgoutput decoding or replay
is supplied. Message/result bounds do not limit caller-retained chunks.

## Large Objects

`pg::lo` exposes create/open/read/write/seek/tell/truncate/close/remove plus
64-bit positioning. Use an explicit transaction; no implicit begin/commit/retry
is added. Resolve built-in function OIDs through the engine, not hardcoded OIDs.
Descriptors are server integers, not independently owning RAII files, and become
invalid at transaction end. Read owns output or borrows the supplied span;
write-all handles chunks. Truncation does not change current position.

Blocking client-file import/export accepts native Unicode paths. Export truncates
its destination and failure can leave a partial file; it is not atomic publication.
File paths refer to the client, not the PostgreSQL host. Rollback handles failed
database changes, not cleanup of application files. No portable async file API
is introduced by these helpers.

## Blocking Counterparts

BlockingPipeline and BlockingExchange use the same state machines with Result
methods. Sequential blocking writes followed by reads can fill both socket
directions; use coordinated async drivers for substantial bidirectional traffic.
Blocking mode does not bypass leases, phase checks, synchronization or limits.
