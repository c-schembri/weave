# Large Objects

Include `<weave/postgres/large_object.hpp>` and use `weave::pg::lo`.
The same operations accept a `Connection` and return Tasks, or accept a
`BlockingConnection` and return synchronous Results.

```cpp
// The caller owns the surrounding SQL transaction.
weave::Task<weave::u32> write_object(
  weave::pg::Connection &database,
  std::span<const std::byte> bytes)
{
  auto object = co_await weave::pg::lo::create(database);
  auto descriptor = co_await weave::pg::lo::open(
    database, object, weave::pg::lo::Access::read_write);

  co_await weave::pg::lo::write_all(database, descriptor, bytes);
  co_await weave::pg::lo::close(database, descriptor);
  co_return object;
}
```

Call inside an explicit, active SQL transaction. No helper implicitly begins,
commits or retries a transaction. Descriptors are server resources, not RAII
objects: close them explicitly, or let transaction completion release them.
They are invalid after transaction/subtransaction rollback or completion and
may be reused by the server. Never retain a descriptor across transactions or
externally close it and then reuse its old value. Object OIDs, unlike descriptors,
identify persistent objects after a successful commit.

- `create(database, requested_oid = 0)` allocates an object, optionally with an
  explicit OID. Existing OIDs fail; zero selects a server-assigned OID.
- `open(database, oid, access)` returns a descriptor. Choose `Access::read`,
  `Access::write` or `Access::read_write`.
- `read(database, descriptor, size)` returns owning bytes. The span overload
  fills a caller-owned buffer and returns its received size; keep the buffer alive
  through the await. Empty output means EOF.
- `write` returns bytes written; `write_all` chunks writes and handles progress.
  Borrowed source spans must remain valid through completion.
- `seek`, `tell` and `truncate` use 64-bit positions. Seek origins are start,
  current and end. Truncation does not change the descriptor's position.
- `close` releases a descriptor; `remove` unlinks an object.

Operations use native FunctionCall messages with binary arguments/results.
Built-in function OIDs are resolved through schema-qualified catalog names once
per connection, not hard-coded. Message limits also bound chunk sizes. There is
no libpq linkage or server-side filesystem access.

`import_file(blocking_database, client_path, requested_oid = 0)` and
`export_file(blocking_database, oid, client_path)` access the **client's** local
filesystem, including native Unicode Windows paths. They are genuinely blocking
helpers, with checked file reads/writes/flushes and bounded transfer buffers.
Export truncates an existing destination and may leave a partial file on failure;
it is not an atomic filesystem replacement. No file is implicitly deleted on
error. Async connections can transfer over their own awaitable source/destination
using `read` and `write_all`; a portable asynchronous file transport is not yet
provided.

After failed imports/exports or SQL operations, roll back the surrounding
transaction before reuse. File errors need not mark PostgreSQL's transaction
failed, but an import may already have created a partial object. Cleanup only
attempts descriptor close when the session is usable; it does not commit or hide
the original error. Cancelled/failed protocol exchanges retain the Connection's
normal terminal/drain rules.

Real-server gates cover ordinary (non-superuser) access, binary round trips,
chunked writes, sparse offsets beyond 2 GiB, truncation position, explicit OIDs,
Unicode client file imports/exports and file failures. Release qualification and
matched large-object benchmarks remain separate requirements.

Reference: [PostgreSQL client large-object interfaces](https://www.postgresql.org/docs/18/lo-interfaces.html).
