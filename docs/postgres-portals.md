# PostgreSQL Portals

Portals are server-side execution objects. `open_portal`, `fetch` and
`close_portal` expose bounded execution inside an explicit transaction;
`describe_portal` inspects an existing portal without fetching or advancing it.
These operations use the connection's existing protocol engine and ownership
rules, not a separate reader or worker thread.

```cpp
co_await database.query("BEGIN");
co_await database.open_portal("items", "SELECT id FROM items ORDER BY id");

auto metadata = co_await database.describe_portal("items");
auto page = co_await database.fetch("items", 64);

co_await database.close_portal("items");
co_await database.query("COMMIT");
```

Check `page.suspended` to decide whether another fetch is needed. Close or finish
ordinary portals before committing. SQL-created cursors can also be described;
their transaction/hold behavior remains PostgreSQL's, not a new Weave policy.

## Description

`Connection::describe_portal(std::string name)` returns `Task<ResultSet>`;
`BlockingConnection::describe_portal` returns synchronous `Result<ResultSet>`.
The empty name describes the current unnamed portal. An unnamed portal may be
replaced by later extended-query work or destroyed by a simple query; inspect it
before issuing work that changes its server-side lifetime.

The result owns column names, table/attribute identifiers, type OIDs, sizes,
modifiers and formats. It has no rows or execution command tag. A portal that
returns no rows produces an empty descriptor through the protocol's NoData
response. This does not mean the portal was executed or completed. Metadata
remains usable after later operations, portal closure and connection destruction.

Formats describe the portal's bound output. Do not infer them solely from
`DECLARE BINARY`: matched PostgreSQL 18/libpq controls observe text metadata for
that SQL cursor's description while its `FETCH` returns binary values. Native
extended-query portals preserve their selected text/binary formats. Decode the
format on the actual result being consumed.

## Ownership And Errors

Description owns the name and retains a connection borrow before its initial
suspension, including a deferred Task. Reset, competing operations and an active
Pipeline lease cannot bypass that admission guard. Pipeline's existing
synchronous `describe_portal` remains the queueing API for batched work.

Invalid names fail before wire submission. Missing portals produce the server's
SQLSTATE and Diagnostic; the response drains through ReadyForQuery, preserving
transaction state. Roll back a failed transaction before continuing. Cancellation
or malformed pending responses are terminal and drain native operations before
cleanup. Description does not make Connection thread-safe.

The reader requires exactly one valid RowDescription or empty NoData response.
Missing/duplicate descriptions, data/command/completion messages, malformed
columns and resource-limit violations cannot become synthetic successful results.

[Protocol flow](https://www.postgresql.org/docs/18/protocol-flow.html#PROTOCOL-FLOW-EXT-QUERY),
[libpq description](https://www.postgresql.org/docs/18/libpq-exec.html) and
[qualification evidence](postgres-qualification.md) document the reference and
tested scope. Full deployment qualification and libpq parity remain open.
