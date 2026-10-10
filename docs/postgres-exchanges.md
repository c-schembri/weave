# Mixed Query and COPY Exchanges

The mixed exchange API is implemented and correctness-qualified on Windows and
Linux in Debug, Release and ASan. Independent malformed/duplex peers, blocking
COPY/backup operations, real logical streams and four-worker cases pass. This is
not a full PostgreSQL production-readiness or libpq-parity claim; see the
[qualification record](postgres-qualification.md).

`Connection::exchange(sql)` sends one simple-query command and returns an owning
session lease. `next()` yields an owning `ExchangeEvent`, in wire order:

- `ResultSet`: ordinary rows or a command completion.
- `CopyFormat`: a new input, output or bidirectional COPY phase.
- `std::vector<std::byte>`: one raw incoming CopyData payload.
- `CopyDone`: the server ended the current receive direction, not the query.
- An empty optional: ReadyForQuery was consumed and the entire exchange ended.

```cpp
auto backup = co_await connection.exchange(
  "BASE_BACKUP (CHECKPOINT 'fast', MANIFEST 'yes')");

while (auto event = co_await backup.next()) {
  if (auto data = std::get_if<std::vector<std::byte>>(&*event)) {
    co_await consume_backup_payload(*data);
  } else if (auto result = std::get_if<weave::pg::ResultSet>(&*event)) {
    inspect_backup_metadata(*result);
  }
}

if (auto status = backup.finish(); !status)
  co_await weave::fail(status.error());
```

The connection must use the appropriate replication startup mode for replication
commands; the server still enforces privileges. The transport neither interprets
archives/manifests nor creates files or acknowledges durable WAL processing.
Replication metadata can lack individual CommandComplete messages, so its
ResultSet command tag may be empty. No synthetic command tag is invented.

For an input/bidirectional phase, `write(span)` sends CopyData and
`finish_send()` sends CopyDone. An optional error string sends CopyFail for input
COPY; it is unsupported for bidirectional COPY. Continue calling `next()` through
the terminal results and ReadyForQuery. Server CopyDone never implicitly closes
application sends. Complete sends are serialized; one reader and coordinated
writers may run through joined children of the same serialized coroutine graph.
Write spans must survive their awaits. Deferred writers retain their phase
generation and cannot silently send into a later COPY phase.

`finish()` is synchronous ownership release, not network shutdown. It requires
ReadyForQuery and no deferred/active method Tasks. Until it succeeds, ordinary
commands and reset are rejected. The Connection and execution owner must outlive
the exchange; the exchange must outlive its method Tasks. Moving the exchange
keeps its private state stable. Dropping an unfinished exchange closes the
connection rather than leave a partial protocol reusable.

Ordinary SQL errors drain ReadyForQuery before propagation. Active bidirectional
stream failures remain terminal; input error recovery also refuses to race active
or deferred senders. Malformed packets, limits and interrupted active I/O make
the connection terminal. Result buffering is bounded per retained result;
returned events belong to the caller, whose own retention is outside that bound.

`BlockingConnection::exchange()` returns `BlockingExchange`, using the same
state machine with synchronous Result methods. Large bidirectional workloads
need the asynchronous interface with coordinated reading and writing; sequential
blocking writes can fill both socket directions.

PostgreSQL can enqueue a final, structurally valid replication keepalive after
CopyDone. Both COPY engines tolerate that control-only tail without implicit
acknowledgement. Arbitrary late payloads are still protocol errors.

Protocol references: [replication commands and backup streams](https://www.postgresql.org/docs/18/protocol-replication.html)
and [logical streaming](https://www.postgresql.org/docs/18/logicaldecoding-walsender.html).
