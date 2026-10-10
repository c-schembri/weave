# PostgreSQL Replication Transport

`Options::replication` explicitly selects `Replication::physical` or
`Replication::database`; the default is `disabled`. Authentication, verified
TLS/mTLS, channel binding, host selection and cancellation use the normal engine.
Physical mode requires `TargetSession::any`: SQL-based session classification is
not appropriate for a physical walsender. Database mode permits ordinary SQL as
well as database-scoped replication commands. The server still enforces privileges.

Both modes require the simple query protocol. Extended execute/prepare/describe,
portals, fast-path function calls, batching and pipeline creation return
`operation_not_supported` locally without sending those protocol messages or
closing an otherwise healthy session. Database-mode SQL must use `query` or
simple-query row streaming, not `execute`. See
[PostgreSQL's replication connection rules](https://www.postgresql.org/docs/18/libpq-connect.html).

Use simple `query` for commands such as `IDENTIFY_SYSTEM` and `start_copy` for
`START_REPLICATION`. `CopyFormat::direction` distinguishes input, output and
both. Its format/columns describe COPY metadata, not the encoding of an opaque
replication payload.

## Independent Directions

In COPY BOTH, `write_copy` sends one raw CopyData payload and `read_copy` returns
one owning raw payload. One reader and serialized writers may progress together
through joined children of the same serialized coroutine graph. The send gate
preserves complete message boundaries; no mutex spans native I/O. Do not share
the Connection between unrelated roots/threads or run ordinary queries during
the stream. Keep write spans alive through their awaits and join every borrower.

Deferred COPY method Tasks capture the current COPY phase when constructed.
A read, write or completion Task from an older phase returns `invalid_argument`
rather than consuming, sending into or terminating a later COPY operation.

`finish_copy_send()` sends CopyDone without reading, and is idempotent while
COPY BOTH remains active. Further writes are rejected; incoming data remains
readable. A server CopyDone ends only the receive direction: `read_copy` returns
an empty optional, but writes may continue until explicitly finished. It does
not silently send CopyDone or truncate pending application work.

```cpp
weave::Task<void> stop_replication(weave::pg::Connection &database)
{
  co_await database.finish_copy_send();

  while (co_await database.read_copy()) {
    // Deliberately discard data still arriving during shutdown.
  }

  co_await database.end_copy();
}
```

`end_copy()` closes the send direction if necessary, discards any unread
CopyData, and drains terminal results through ReadyForQuery. It requires
exclusive read ownership; join a separate reader first. The retained
`copy_results()` includes both command results and any timeline-switch rows;
`copy_result()` exposes the last result for existing single-result callers.
Neither is available before terminal draining. A clean completed stream can
reuse the same replication connection for another command.

Unlike COPY BOTH, ordinary COPY OUT still returns EOF only after its command
and ReadyForQuery have been drained. COPY IN retains its existing `end_copy`
and CopyFail behavior. Passing an error message to COPY BOTH `end_copy` is
unsupported; use explicit cancellation/closure rather than assuming recovery.

Malformed frames, stream errors, resource overflow and cancellation after I/O
begins make COPY BOTH terminal. SQL diagnostics remain inspectable. Weave does
not attempt Sync-based recovery while a separate sender may still be active;
join/drain borrowers before reset or destruction. `Connection::finish` rejects
active COPY BOTH rather than racing its sender with a Terminate message.

## Scope

This is raw replication transport, not a database replica or logical decoder.
Callers interpret XLogData, keepalives and plugin-specific bytes, schedule status
updates, manage slots, and acknowledge only data actually persisted/applied.
There is no automatic acknowledgement, durable WAL storage, timeline following,
feedback timer, pgoutput decoder or automatic replay. Message payloads are bounded
by `Limits::message_bytes`; retained terminal results by `result_bytes`. Caller
retention of returned chunks is outside those internal bounds.

The blocking facade uses the same state machine and synchronous Result methods.
Large bidirectional traffic needs the async API with coordinated sending and
reading; a blocking send followed by a read can fill both socket directions.

Qualification covers physical streaming, database-mode startup/SQL, verified
TLS/mTLS/PLUS, terminal results, half-close, malformed messages and four-worker
execution. The [mixed exchange API](postgres-exchanges.md) also passes real
BASE_BACKUP through Task/blocking APIs and built-in pgoutput streaming through
Task/multicore gates on Windows/Linux; optional test_decoding passes where installed. Both COPY
APIs are tested for logical stream termination and connection reuse. Old-timeline
transitions on a real server, archive restoration, complete replica workflows
and final release qualification remain open. See the
[parity matrix](postgres-parity.md), not a blanket replication-parity claim.

Protocol references: [COPY message flow](https://www.postgresql.org/docs/18/protocol-flow.html#PROTOCOL-COPY)
and [streaming replication](https://www.postgresql.org/docs/18/protocol-replication.html).
