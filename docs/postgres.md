# PostgreSQL

The optional `weave::postgres` module implements PostgreSQL's wire protocol;
it does not wrap or link libpq. Its authentication uses OpenSSL Crypto and ICU
SASLprep. It depends on TLS, TCP, IO and Sync, not Runtime.

The implemented scope is experimental. The [closed checkpoint](postgres-release.md)
and [parity checklist](postgres-parity.md) define its scope, not complete libpq compatibility.
The [individual API audit](postgres-api-audit.md) records concrete mappings and gaps.
The [latest libpq measurements](postgres-benchmarks.md) record throughput, CPU,
tail latency and its measurement limitations separately from correctness gates.
[Current validation and scope](postgres-closure.md) / [historical qualification and local gates](postgres-qualification.md).

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
  auto rows = co_await database.execute("SELECT $1::int", parameters);
  auto number = rows.rows.front().front().integer<int>();
  if (!number)
    co_await weave::fail(number.error());

  co_await database.finish();
}
```

TLS is verified by default. Set `Options::tls` to an immutable client credential
snapshot for private CAs or mTLS. Ordinary defaults never fall back to
unauthenticated transport; [weaker TLS modes](postgres-connections.md#tls-modes)
require explicit opt-in and retain their documented fallback restrictions.
`plaintext = true` explicitly opts into unencrypted transport; it is intended
for trusted local deployments and matched benchmarks. Cleartext password
authentication requires verified TLS and an explicit opt-in; deprecated MD5
authentication also requires an explicit opt-in. SCRAM-SHA-256 and
SCRAM-SHA-256-PLUS verify the server proof. `ChannelBinding::require` rejects
non-SCRAM and non-bound authentication, including trust authentication.

`query` accepts simple SQL and returns every result set, including multiple
statements. `execute` uses Parse/Bind/Describe/Execute with owned text, binary or
NULL parameters. `prepare`, `execute_prepared`, `describe` and `close_prepared`
expose named prepared statements. Column metadata includes OIDs and formats;
`Value::integer` decodes **text** integers only; `binary_integer<T>` explicitly
decodes a network-order integer of matching width. Binary bytes are not silently
interpreted as text. SQLSTATE is preserved in the error category, while
`last_error()` retains the server diagnostic fields on a surviving connection.
`last_failure()` returns an owning, synchronous snapshot of the admitted
operation's local/SQL cause and retained diagnostic, including after transport
closure. Inspect it only after session Tasks and leases have drained;
[failure scope and formatting](postgres-diagnostics.md#session-failures).
`ResultSet::kind` distinguishes commands, tuple results, empty queries,
descriptions and partial chunks independently of row count. For owning SQL
failure data, use `query_outcomes`, `execute_outcome` or `execute_prepared_outcome`;
the blocking facade provides the same methods. Default query/execute methods
still propagate SQL errors through Task/Result.
[Result kinds, ownership and retained diagnostics](postgres-results.md).
For startup failures, `connect(options, diagnostic)` fills a caller-owned
`Diagnostic`; keep it alive through the await. Transport failures leave it empty.

`call_function(oid, parameters, format)` implements PostgreSQL's legacy fast-path
FunctionCall protocol and returns an owning, nullable `Value`. Argument types
come from the server function, so `Parameter::type` must be zero on this path;
text/binary formats still apply. Resolve application function OIDs through a
query rather than assuming that extension OIDs match between databases. SQL
errors drain through ReadyForQuery just like ordinary queries.

Prefer bound parameters for data values. Where SQL construction really needs
quoting, synchronous `escape_literal` and `escape_identifier` validate the current client encoding,
reject embedded NUL/invalid sequences and return fully quoted strings. Literal
quoting uses explicit escape-string syntax, independent of
`standard_conforming_strings`. Complete multibyte characters retain their
continuation bytes; the helpers are not generic encoding converters.
`encode_bytea` produces PostgreSQL hex text (not a SQL literal);
`decode_bytea` accepts hex and legacy octal/backslash representations. Both own
their output and enforce an explicit output-size limit. Do not double-escape
values passed through query parameters.
Keep the client encoding unchanged between quoting and executing the resulting SQL.
[Encoding control, validation and quoting policies](postgres-encoding.md).

`change_password(user, password)` generates a verifier and changes the password
without putting plaintext into SQL. Synchronous and connection-policy verifier
utilities are also available. [Password policy, ownership and failure](postgres-passwords.md).

Synchronous `info()` returns an owning snapshot of the selected destination,
login/database, negotiated protocol, transaction state and actual TLS/GSS
transport. It requires an idle session and exports no secrets or native handles.
[Metadata fields, lifetime and compatibility](postgres-metadata.md).

`pg::lo` provides transaction-bound large-object operations, including 64-bit
positions and checked client-file import/export on the blocking facade.
[Large-object APIs and ownership](postgres-large-objects.md).

`Options.hosts` selects destinations, with optional ordered/random traversal and numeric address
pinning. `target_session` selects writable/read-only, primary/standby or preferred
standby sessions. `reset(options)` explicitly replaces a session without replaying
SQL or retaining its old authentication password.
`Options::parse` synchronously accepts a documented URI/keyword subset without
environment or file access. Explicit `Options::load` resolves environment,
service and per-host password files during setup.
[Connection strings, configuration loading, selection and reset](postgres-connections.md).

`batch` pipelines extended queries with one final Sync. It sends and receives
concurrently to avoid socket-buffer deadlocks. A SQL error is returned in that
command's `Outcome`; subsequent commands are marked `aborted` until Sync.
Earlier successful results remain available. PostgreSQL transaction semantics
still apply: successful commands in an implicit pipeline transaction may roll
back when a later command fails. No automatic retry is performed.

`pipeline()` reserves an explicitly managed extended-protocol session. Queue
commands and Sync barriers synchronously, `co_await flush()` for duplex progress,
and consume correlated owning results with `next()` or synchronous `try_next()`.
`finish()` releases it after synchronization, result draining and Task cleanup.
[Pipeline boundaries, incremental row chunks, recovery and ownership](postgres-pipelines.md).

COPY uses `start_copy`, `write_copy` / `read_copy` and `end_copy`. Reads own each
returned chunk; write spans must survive their await. Ordinary COPY OUT returns
an empty optional after CommandComplete and ReadyForQuery; `copy_result` then
exposes the command tag. COPY IN `end_copy(message)` sends CopyFail and drains
the server error. Other commands are rejected while COPY is active.

Explicit replication startup and raw COPY BOTH support independent sending and
receiving. `finish_copy_send` closes only the send direction; receive EOF closes
only the receive direction. `end_copy` drains the terminal `copy_results` before
connection reuse. [Replication transport and remaining limits](postgres-replication.md).

Mixed result/COPY commands use the [owning exchange-stream API](postgres-exchanges.md).
It preserves owning events and explicit phase boundaries rather than hiding
backup metadata or collecting unbounded raw streams.

`request_cancel` sends a PostgreSQL CancelRequest through a separate connection
to the selected server endpoint. Calling it captures an owning backend snapshot
immediately; the lazy Task does not borrow the query Connection. Reset, move,
finish or destruction cannot retarget an already-created cancellation Task.
Calling it on a closed connection creates a Task that fails with `Error::closed`,
even if that connection is reset before the Task runs.
There is no success acknowledgement or rollback guarantee: the command might
already have completed. Await the original command and inspect its result.
`cancel()` instead cancels transport work and makes the connection terminal.

Connections and inspection methods are single-execution-owner objects, not
thread-safe shared sessions. Concurrent ordinary commands fail with `busy`.
Connection moves/destruction require no active operations; the Connection and
its Context must outlive every task borrowing them. PostgreSQL cancellation is
the exceptional independent operation, not a second query reader. Obtain an
owning `cancel_handle()` before sharing cancellation across threads; its
`request()` owns the snapshot even after the handle is destroyed, while
`request_blocking()` drives a separate calling-thread Context. Copying a handle
shares immutable cancellation credentials, not the query Connection.
The blocking method must run outside an executing Context. Requesting through a
moved-from CancelHandle is a fatal contract violation. Independent requests may
run concurrently on separate execution owners; they never read the query stream.
[Cancellation deadlines, transport and snapshot boundaries](postgres-connections.md#cancellation).
Moved-from connections support destruction, `open`, `close` and `cancel`;
`info()`, `cancel_handle()` and `request_cancel()` report `Error::closed`. Other operations
require a live implementation and fail the contract immediately.

Ordinary SQL errors drain ReadyForQuery before propagation, preserving connection reuse.
Errors during active COPY BOTH are terminal; its separate sender may still be active.
Transport errors, malformed messages, resource-limit failures and task-token
cancellation after an exchange begins close the connection rather than leave a
half-consumed protocol reusable. `as_result` can recover from SQL errors; an
explicit transaction in failed state still needs `ROLLBACK`.

Message sizes, retained results, notification queues and SCRAM iteration work
are bounded by `Limits`. These are protocol/retained-data limits, not an exact
allocator accounting promise. Results, notifications and notices own their data.
Take queued notices/notifications regularly; overflow fails rather than silently
discarding messages. `wait_notification()` is an exclusive idle reader; do not
start queries on that connection while waiting.

`on_notification` optionally consumes newly decoded notifications through an
owning `noexcept` callback. It does not create a reader: normal operations or an
explicit `wait_notification()` must drive input. The wait still returns its
owning event, and pre-existing queued events are not replayed through callbacks.
[Notification delivery, save/restore and subscription lifetime](postgres-notifications.md).

An optional owning `noexcept` notice handler receives validated server diagnostics
inline instead of queuing them. Pass it to `connect` to observe startup notices,
or replace it synchronously with `on_notice`; reset retains the registration.
[Notice callbacks, save/restore and lifetime rules](postgres-notices.md).

`on_event` registers owning `noexcept` connection/result lifecycle observers
with separate application-state slots. Result copies explicitly clone/share
state; retained results do not implicitly keep their Connection alive. Scoped
regression qualification passed; see [lifecycle events and ownership](postgres-events.md).

`on_trace` installs a separate owning `noexcept` protocol observer. Metadata is
the default; application bodies require explicit bounded opt-in. Startup,
authentication and backend cancellation keys are always withheld.
[Protocol tracing, disclosure policy and execution contracts](postgres-tracing.md).

`start_rows` / `read_row` consume one owning row at a time, including multiple
simple-query results. `row_columns()` borrows the current result's metadata until
the next read changes it. `for_each_row(sql, handler)` awaits each Task callback
before reading again and closes the connection if the callback fails.
`open_portal` / `fetch` / `close_portal` expose bounded server-side chunks inside
an explicit transaction. A fetched result's `suspended` flag means another fetch
is needed; finish or close the portal before committing the transaction.
`describe_portal(name)` returns owning column metadata without executing or
advancing a named or unnamed portal. The blocking facade exposes the same method.
[Portal lifetime, description and format contracts](postgres-portals.md).

`pg::BlockingConnection::connect` owns a calling-thread Context and drives the
same protocol engine with `run`. It returns synchronous `Result` values and
does not create I/O worker threads. Explicit GSS providers use their own bounded
credential workers. Use `pg::connect` inside a Context or Runtime
for nonblocking coroutine execution. The blocking facade must remain on its
creating thread; it is not an independently implemented protocol client.
Do not drive its blocking operations from inside an executing Context/Task;
use the nonblocking Connection there rather than recursively pumping a loop.
The blocking facade exposes the same diagnostic factory overload, inspection,
notice/notification queues, COPY/row metadata, quoting and fast-path operations.

Build with `-DWEAVE_MODULES=postgres`. Consumers use
`find_package(weave CONFIG REQUIRED COMPONENTS postgres)` and link
`weave::postgres`. OpenSSL 3.5+ and ICU 70+ are required only by selected
components. Library-only builds do not fetch libpq, test frameworks or servers.
Building PostgreSQL from source also requires a C compiler for its private,
vendored llhttp/uriparser engines; installed C++ consumers do not. Their headers
and CMake targets are not exported, and their licenses are installed with Weave.
On Linux, the native GSSAPI backend defaults on and requires the
`krb5-gssapi` and `krb5` pkg-config packages (MIT Kerberos development files).
Set `WEAVE_POSTGRES_GSSAPI=OFF` to build without that dependency. Windows links
its system SSPI library. [GSS/SSPI authentication and encryption](postgres-gss.md) use an explicitly
created, bounded provider context with captured identity and cancellation-safe
offload. GSS encryption is opt-in; Linux real-server interoperability is qualified,
while positive Windows domain/Kerberos encryption still needs deployment evidence.

[OAuth](postgres-oauth.md) supports custom providers and native HTTPS device
authorization, protected PostgreSQL discovery and endpoint-pinned reconnects
through Task/blocking facades. The native provider uses application-owned prompts
and credentials, with an explicit synchronous cached-token lookup. An opt-in Linux
Keycloak deployment gate covers real signed tokens and server introspection;
full libpq OAuth parity, broader deployments and matched OAuth measurements remain pending.

Protocol reference: [PostgreSQL 18 message flow](https://www.postgresql.org/docs/18/protocol-flow.html)
and [SASL authentication](https://www.postgresql.org/docs/18/sasl-authentication.html).
