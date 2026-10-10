# PostgreSQL Pipelines

`Connection::pipeline()` synchronously returns an owning `Result<Pipeline>`.
It reserves the connection for extended-protocol work and rejects active
exchanges, deferred session Tasks and COPY/row readers. `batch` remains the
simpler one-Sync convenience API.

```cpp
weave::Task<void> requests(weave::pg::Connection &database)
{
  auto pipeline = database.pipeline();
  if (!pipeline)
    co_await weave::fail(pipeline.error());

  auto command = pipeline->execute({"SELECT $1::integer", {{"42", 23}}});
  if (!command)
    co_await weave::fail(command.error());

  auto barrier = pipeline->sync();
  if (!barrier)
    co_await weave::fail(barrier.error());

  co_await pipeline->flush();

  while (auto result = co_await pipeline->next()) {
    if (result->id == *command && result->outcome.result) {
      // Own and inspect the result rows here.
    }
    if (!result->outcome.error.fields.empty()) {
      // Inspect the owning SQL diagnostic here.
    }
  }

  if (auto finished = pipeline->finish(); !finished)
    co_await weave::fail(finished.error());
}
```

## Commands And Boundaries

SQL/Sync queueing is synchronous, performs no socket I/O and returns monotonically
increasing correlation IDs. Results preserve submission order, IDs and kinds.

- `execute(Command, RowOptions = {})` queues Parse/Bind/Describe/Execute for one SQL statement.
- `prepare`, `execute_prepared`, `describe` and `close_prepared` operate on named
  statements, including a prepare immediately followed by its execution.
- `describe_portal` and `close_portal` operate on existing named portals, with
  their normal transaction lifetime. This API does not create portals.
- `sync()` queues a protocol Sync. Its result has kind `sync` and a `transaction`
  status, not a fabricated query result or command tag.
- `flush()` snapshots queued requests, appends backend Flush, and concurrently
  writes requests and reads correlated responses. It drains both sides and
  **does not add Sync**. Commands queued while that snapshot runs belong to the
  next explicit flush.

Unlike libpq's transport-only `PQflush`, Weave's `flush` includes response
draining and sends the backend Flush request. No polling thread or Runtime is required.

## Queueable Flush

`Pipeline::request_flush()` and `BlockingPipeline::request_flush()` synchronously
queue a backend Flush at the current position in the outgoing request buffer.
They return `Result<void>`: no socket I/O, implicit Sync, correlation ID, admission
slot or result event is produced. The five-byte message counts against the queued
byte limit. A rejected request leaves the queue unchanged.

```cpp
if (auto requested = pipeline.request_flush(); !requested)
  co_await weave::fail(requested.error());
```

Use the existing send/receive/flush operations to drive transport afterwards.
Their automatic trailing Flush remains unchanged, so an explicit marker can be
followed by another trailing Flush. Flush-only snapshots are sent even without
SQL/Sync entries; they require no response. They do not create unsynchronized
SQL work or clear an aborted pipeline. Only a valid Sync response clears abort
state, and outstanding SQL still needs synchronization before finish.

An unsent marker prevents `finish()`. Dropping a wholly unsent pipeline discards
it without sending anything. Once split receive has reserved outgoing bytes,
the sender must still send them before finish, even if no result is expected.
The blocking facade retains its existing active-driver/sent-window busy checks.

Permanent Windows/Linux Debug, Release, ASan and reduced-module controls pass,
including cancellation, EOF, malformed responses, real PostgreSQL over plain TCP
and verified mTLS, independent libpq controls and relocated component packaging;
[exact qualification and retained fixture failures](postgres-qualification.md#queueable-flush-regression-qualification-2026-10-09).
This is an owning queueing API, not libpq's polling/partial-write lifecycle.

## Separate Sending And Receiving

`send()` writes a bounded snapshot and backend Flush without reading query
responses or adding Sync. Completion means the transport accepted all encoded
bytes, not that PostgreSQL received, executed or committed them. Several sends
may complete before reading earlier commands. Command IDs and admission remain
occupied until their complete results are consumed.

`receive()` reads a snapshot of submitted commands into the result queue, without
writing. `next()` consumes that queue; it does not read the socket itself. A
receiver started first may reserve queued commands for its partner sender, so
joined send/receive Tasks need not depend on scheduling order. Later commands
queued after reservation belong to another send/receive snapshot. An empty
snapshot completes immediately; this is not an idle notification poll.

One sender and one protocol receiver may run together, alongside one result
consumer in the same serialized coroutine graph. A second active sender or
receiver fails with `busy`. `flush()` refuses to mix with split operations or
submitted/unwritten commands; finish the split exchange before switching modes.
No background driver is started implicitly.

For small bounded requests, `co_await pipeline.send();` followed by
`co_await pipeline.receive();` and then result consumption is supported. It is
not a general bulk pattern: sending alone can block while PostgreSQL is blocked
writing results. For large traffic, overlap both directions and the consumer:

```cpp
co_await weave::scope([&](weave::TaskScope &tasks) -> weave::Task<void> {
  auto transport = tasks.spawn(weave::when_all(pipeline.send(), pipeline.receive()));
  if (!transport)
    co_await weave::fail(transport.error());

  co_await consume(pipeline);
});
```

Here `consume` is the result consumer below. `when_all` keeps both transport
directions in one serialized producer graph even on a work-stealing Runtime;
do not spawn them as separate stealable roots sharing a Connection. On consumer
failure, scope cleanup cancels and drains both directions. Keep the Pipeline/Connection outside the
scope body. Incremental receives require a progressing consumer, just like
incremental `flush()`. Cancellation of either active transport direction is
terminal and wakes the other direction; cancellation of a waiting `next()` alone
still does not cancel transport work. Unstarted Tasks only retain their leases.
An earlier Sync acknowledgement never clears the synchronization debt of a
later submitted command.

Several Syncs may be queued together. A successful command result is not a
commit acknowledgement: an implicit transaction can roll back when a later
command fails. There is no automatic SQL replay or retry.

## Results And Recovery

`PipelineResult::complete` is true for a completed command or Sync. Buffered
commands (the default) still produce one complete result containing all rows.

Opt into incremental results per command with `{.chunk_rows = 128}`. A value of
one is single-row delivery; zero retains buffered behaviour. Every chunk owns
its schema and rows, has the command's ID and kind, and has `complete = false`.
Chunks contain between one and `chunk_rows` rows; the retention budget can
force an earlier boundary. Successful completion is a separate zero-row result
with `complete = true` and the command tag. Empty queries/results have no chunks.
Prepared commands accept the same final `RowOptions` argument, including binary
results and NULL values. Descriptions, closes and Syncs remain complete results.

Streaming requires a consumer progressing alongside `flush()` or `receive()`. The bounded
delivery queue and byte budget apply backpressure instead of accumulating the
entire query. Awaiting streaming `flush()` alone can therefore wait indefinitely.
Use joined lifetime ownership, including failure cancellation:

```cpp
weave::Task<void> consume(weave::pg::Pipeline &pipeline)
{
  while (auto event = co_await pipeline.next()) {
    if (!event->outcome.error.fields.empty())
      co_await weave::fail(weave::pg::sql_error(event->outcome.error.sqlstate()));

    if (!event->complete) {
      // Process the owning row chunk here.
    }
  }
}

weave::Task<void> stream(weave::pg::Pipeline &pipeline)
{
  auto command = pipeline.execute(
    {"SELECT * FROM generate_series(1, 100000)"}, {.chunk_rows = 128});
  if (!command)
    co_await weave::fail(command.error());
  if (auto barrier = pipeline.sync(); !barrier)
    co_await weave::fail(barrier.error());

  co_await weave::scope([&](weave::TaskScope &tasks) -> weave::Task<void> {
    auto producer = tasks.spawn(pipeline.flush());
    if (!producer)
      co_await weave::fail(producer.error());

    co_await consume(pipeline);
  });

  if (auto finished = pipeline.finish(); !finished)
    co_await weave::fail(finished.error());
}
```

Include `<weave/scope.hpp>` for this pattern. A failed consumer cancels and
drains the scoped producer before releasing its borrowed Pipeline. `when_all`
alone joins children but does **not** cancel siblings on failure; do not leave
a failed consumer's producer parked on backpressure.
Successful scope exit joins rather than cancels: consume through EOF, or request
scope cancellation for an intentional early exit. Keep the Pipeline/Connection
outside the scope body, as above, or explicitly join before their body-local
owners leave scope. Scope cleanup does not extend successfully returned locals.

Already delivered chunks can be followed by a SQL error. The unfinished chunk
is discarded, not reported as success; published rows cannot be retracted.
Application effects must remain provisional until command/transaction success.
Unlike libpq's final partial-chunk tag placement, Weave puts the tag only on the
explicit complete event.

SQL errors are values in `outcome.error`. Subsequent commands have
`outcome.aborted = true` until Sync, including across separate flush calls.
Sync resumes protocol processing but does **not** repair a failed explicit
transaction: queue `ROLLBACK` after the barrier. `aborted()` reports protocol
skip-until-Sync state, not transaction health.

`next()` waits for outstanding requests, including queued but unflushed work.
Flush first, or run a flusher and consumer concurrently with joined lifetime
ownership. An empty optional means all outstanding results were consumed, not
permanent closure; queueing more requests allows further reads. Only one awaited
consumer may be active; a second returns `busy`.

`try_next()` is genuinely synchronous. It returns a ready result, an empty
optional when no work is outstanding, or `resource_unavailable_try_again` when
progress requires a flusher. It rejects competing with an awaited consumer.
Admission counts queued, executing and unconsumed completed commands, including
Syncs. Consuming a row chunk releases its storage but not its command slot.

Malformed/out-of-order responses, unexpected COPY, retention overflow, transport
failure or cancellation of an active flush make the connection terminal. Already
published valid results remain readable before the stored failure. Cancelling
only a waiting `next()` removes that consumer without cancelling the separate
flusher. Join/drain that flusher before releasing its borrowers.

## Ownership And Limits

`connection.pipeline_status()` synchronously reports `off`, `on` or `aborted`.
It describes the owning lease and observed abort-until-Sync latch, not socket
health or an open/failed SQL transaction. Valid Sync parsing clears `aborted`
before the application consumes buffered results; queueing Sync is insufficient.
Terminal I/O does not release the lease. [Full inspection contract](postgres-metadata.md#pipeline-state).

`finish()` is synchronous, with no implicit flush or Sync. It succeeds only after
synchronization has been acknowledged, all results consumed and all active
**or deferred** pipeline Tasks cleaned up. It releases the connection lease for
ordinary commands/reset. An unused pipeline can finish immediately.
This deliberately differs from native libpq 18.4/18.6, whose exit controls can
succeed after consuming all unsynchronized command results. Weave never releases
a sent pipeline merely because its results have been consumed.

Dropping an idle pipeline discards unsent work. Dropping one after unsynchronized
work or an unrecovered protocol error closes the connection. Moves/destruction
with outstanding pipeline Tasks are fatal lifetime violations. The connection
must outlive an unfinished pipeline, and its Context must outlive transport work.
A finished pipeline no longer borrows the connection. Moved-from pipelines
support destruction only.

`Limits::pipeline_commands` defaults to 1024 and accepts 1 through 65535.
`message_bytes` bounds each encoded command and inbound message. Encoded queued
requests are bounded by `result_bytes`; transport submission adds its existing
five-byte trailing Flush. One active and one newly queued snapshot may coexist.
Shared retained results are also bounded by `result_bytes`;
consumption releases their charges. Streaming charges duplicated schema and
active staging as well as queued deliveries. A single row/schema that cannot
fit even after consuming queued data fails with `resource_limit`, rather than
waiting for the reader to reclaim its own staging storage. These are logical data/container bounds,
not exact allocator capacity or total RSS. Decode/input scratch, connection
diagnostics and administrative queues have separate bounds. Allocation exhaustion
follows the library's existing policy.

All pipeline methods require caller-serialized ownership. Queueing/consumption
may interleave through joined children of the same serialized coroutine graph;
one scoped flusher, or one split sender and receiver, may progress alongside
that consumer. Unrelated roots or
threads must not concurrently register/poll consumers or
operate on the pipeline/Connection. Internal locks do not make the session a
thread-safe shared queue or remove transport execution-domain contracts.

`BlockingConnection::pipeline()` returns `Result<BlockingPipeline>` with the same
queueing, IDs, results and explicit boundaries. Its `flush()` drives the same
engine on its creator-thread Context. Its synchronous `next()` polls results,
returning `resource_unavailable_try_again` before a required flush instead of
waiting for an unstarted writer. Successful `finish()` releases its session and
Context references. Do not invoke blocking flush inside an executing Context.

For blocking incremental consumption, queue the commands and Sync, call
`start()`, and repeatedly call `next()` until its empty optional. `start()` owns
a lazy full-duplex driver; `next()` drives it on the existing calling-thread
Context. No helper thread or background progress is implied. The final empty
optional joins the driver, after which another snapshot or `finish()` is allowed.
Queueing, `start`, `flush` and `finish` reject an outstanding driver with `busy`.
Dropping it cancels and drains the driver before destroying the Pipeline.
Blocking progress and active-driver destruction must occur on the owning thread,
outside an executing Context, just like other blocking operations.

Blocking `flush()` retains its existing completion semantics for buffered work
and rejects queued streaming commands with `operation_not_supported` before any
I/O. Use `start()` for streaming, not a flush that needs a consumer which cannot
run until it returns. `start()` is also available for buffered snapshots.

Blocking `send()` writes without reading and keeps one submitted snapshot open.
Queueing, another send, duplex `start`/`flush` and `finish` return `busy` until
that snapshot is received. `receive()` buffers its results and preserves the
polling `next()` behavior; streaming snapshots reject this method with
`operation_not_supported`. Use `start_receive()` followed by `next()` through
EOF for incremental results. Its owned reader is driven and joined on the same
Context, like `start()`. Dropping a reader cancels/drains it; dropping a sent but
unread snapshot closes the session. Neither method sends queued commands.
Calling receive/start_receive before a successful send returns `busy` rather
than waiting for a sender which cannot run on the blocked caller. These split
blocking methods are for bounded requests; use the duplex `start()` pattern for
bulk work to avoid two-way socket backpressure deadlock.

Simple-query multi-statements and COPY are not pipeline operations. Raw
[COPY BOTH/replication](postgres-replication.md) uses a separate session mode.
`start()` is an owning full-duplex driver, not proof that requests reached the
peer. Server-defined transaction semantics still apply. Native handle/polling
integration and libpq-style partial-write status APIs are not exposed.

[PostgreSQL's pipeline reference](https://www.postgresql.org/docs/18/libpq-pipeline-mode.html)
defines baseline semantics; polling/memory APIs are mapped to owning C++ values.
[Its incremental-results reference](https://www.postgresql.org/docs/18/libpq-single-row-mode.html)
describes chunks, terminal results and errors after delivered rows.
