# PostgreSQL Results

Ordinary `query`, `execute` and `execute_prepared` keep their existing behavior:
SQL failures propagate as Task errors, or synchronous Result errors on the
blocking facade. `sqlstate(error)` decodes the error code; `last_error()` returns
an owning Diagnostic snapshot from the connection.

## Allocation Reporting

`ResultSet::memory_size()` uses owning, counted storage. It is synchronous and
reports retained backing requests, not a capacity estimate. The selected contract
now has permanent Windows/Linux Debug, Release, ASan, reduced-build, native-libpq,
real-server and packaging coverage. This is not whole-module production certification.
[Storage boundary, compatibility changes and evidence](postgres-result-storage.md).

[PQresultMemorySize](https://www.postgresql.org/docs/18/libpq-misc.html) reports
libpq-owned malloc requests, not payload size, allocator usable size, RSS or all
application event-data allocations. Weave includes reachable pool objects, allocation
headers/padding, container/proxy requests and result-local event records. Shared
receiver objects, application event data, caller storage and inspection scratch
are excluded. Shared pools are counted once per result, not exclusively across
several results. The existing retained-data limits remain logical bounds, not
physical allocation quotas.

Parsed results, copies and event records now use the storage model. Row batches
have separate pools so a live exchange does not accumulate discarded chunks.
Six-profile allocation/lifetime, string-format/hash, fatal-guard and owned real-server
controls pass, as do focused Debug regressions and Windows Release/Linux Debug
packaging. Dedicated pressure-pipeline, raw-row and physical-replication ownership
controls also pass all six profiles. The query/container contract is selected:
inspection may allocate temporary bookkeeping; borrowed views keep ordinary
lifetime rules; owning standard-container copies are explicit. Substring and
concatenation yield independent std::string values. Permanent tests cover the
public container boundary and the actual result-producing paths across nine profiles.
PQresultMemorySize is mapped with these ownership/measurement differences explicit;
Matched native Windows retained-memory and performance measurements are now
available in [the benchmark report](postgres-benchmarks.md). They do not measure
malloc call counts, total RSS, or equal exclusive ownership across the two clients.

## INSERT OIDs

`inserted_oid()` returns a synchronous `Result<u32>` and `inserted_oid_text()`
returns a borrowed `Result<string_view>`. Non-INSERT commands return zero/empty;
`INSERT 0 count` returns zero/"0". Malformed, negative or overflowing OIDs fail
rather than silently wrapping. The text view borrows the command tag and keeps
its original spelling. Modern PostgreSQL tables normally report an INSERT OID
of zero; this is command-tag compatibility, not the inserted primary key.

## Display And Application Failures

`ResultSet::format(ResultFormat)` is synchronous and returns an owning
`Result<string>`. `ResultLayout::table`, `delimited`, and `html` support headings,
NULL text, captions and row counts; `expanded` applies only to tables. Binary
values render as hex. HTML markup is escaped and table widths use the selected
PostgreSQL encoding. Delimited output quotes separators and quotes but escapes
controls as `\\xNN`; it is a display format, not lossless CSV serialization.
Input and output default to 1 MiB bounds, with a 64 MiB maximum. No FILE handles,
pager subprocesses or byte-exact legacy libpq display compatibility are exposed.

`Outcome::failure(Diagnostic, limit)` creates a checked, owning application
failure (default bound 1 MiB). It requires a message, validates optional SQLSTATE
and rejects duplicate or invalid fields. It does not fabricate a wire ResultSet,
trigger result lifecycle hooks, or change automatic SQL-error propagation.
An application-created diagnostic is not evidence that a server sent it.

## Standalone Chunks

`Connection::exchange(sql, RowOptions)` and the blocking facade now accept the
same `chunk_rows` option as extended pipelines. Zero retains the existing buffered
query results. A positive value bounds each delivered row chunk; retained-data
pressure can produce a smaller chunk before that row count is reached.

```cpp
auto exchange = co_await database.exchange("SELECT * FROM measurements", {.chunk_rows = 256});

while (auto event = co_await exchange.next()) {
  if (auto result = std::get_if<weave::pg::ResultSet>(&*event)) {
    // Consume owning rows or inspect the terminal command result.
  }
}

if (auto finished = exchange.finish(); !finished)
  co_await weave::fail(finished.error());
```

Ordinary query chunks have `ResultKind::row_chunk`, nonempty rows, complete copied
column metadata and an empty command tag. A successful described command ends
with `ResultKind::tuples`, the actual command tag and zero rows, including queries
that returned no rows. Continue calling next until its empty optional to consume
ReadyForQuery, then finish explicitly. Chunk size one uses the same row_chunk kind,
not a separate status enum. These are owning C++ counterparts of
[libpq's incremental results](https://www.postgresql.org/docs/18/libpq-single-row-mode.html),
not a post-submission mode setter or its C status/command-tag placement policy.

The setting covers every statement in a simple query. Command/empty-query and
COPY format/data/completion events keep their wire order and existing kinds.
Chunks retain their rows and schema after subsequent reads, closure and owner
destruction. Holding returned chunks grows application memory outside the active
staging bound. The engine reserves both staging and delivery schema charges;
limits remain logical data bounds, not exact allocator accounting or total RSS.
One pending wire message remains separately bounded by message_bytes. A single
row or duplicated schema that cannot fit fails with resource_limit.

An SQL error discards undelivered partial rows, drains ReadyForQuery and fails
next with the SQLSTATE error; as_result permits explicit recovery and finish.
Already delivered rows are provisional: applications must discard or undo their
effects if the command ultimately fails. Transport/protocol/resource failures or
cancellation after active reading retire the session. Task/Exchange lifetime,
serialized ownership and explicit finish contracts remain unchanged.
Cancellation before a deferred reader starts leaves pending completion messages
intact; an uncancelled reader can resume and drain the exchange.

Permanent controls pass nine Windows/Linux profiles, including Debug, Release,
ASan, builds without Runtime/LDAP/GSS, real PostgreSQL plain/mTLS sessions and
relocated packaging. They cover withheld-tail delivery, zero-column rows,
metadata/payload ownership, SQL recovery, resource limits, cancellation and
malformed responses in asynchronous and blocking modes. Binary/UTF8/NUL payload
and field-format preservation use synthetic wire fixtures with independent libpq
controls, not a claim that simple SQL queries request binary server output;
[exact evidence](postgres-qualification.md#standalone-exchange-chunks-qualification-2026-10-09).

## Retained Outcomes

Choose the explicit outcome methods when SQL diagnostics must travel with their
results independently of a live Connection:

```cpp
auto outcomes = co_await database.query_outcomes("SELECT 1; SELECT 1/0; SELECT 99");

for (const auto &outcome : outcomes) {
  if (!outcome.result) {
    auto diagnostic = outcome.error;
    // Application policy decides whether to log, recover or propagate.
    continue;
  }

  const auto &rows = outcome.result->rows;
  // Consume the successful result.
}
```

`query_outcomes` returns `Task<std::vector<Outcome>>` in wire order, including
completed successes before a server error. A simple query stops on its first
SQL failure: the example returns a tuple result and a Diagnostic, not a result
for SELECT 99. Weave does not parse SQL to manufacture aborted entries for
unexecuted statements. Explicit batches/pipelines already correlate their queued
commands and have separate `Outcome::aborted` semantics.

`execute_outcome` and `execute_prepared_outcome` return `Task<Outcome>` for one
extended operation, with the same parameters and text/binary format controls as
their normal counterparts. BlockingConnection returns the corresponding
synchronous `Result` types and drives the same engine.

For these methods only, a server SQL failure is an owning Outcome value with
no ResultSet, an owning `error` Diagnostic and `aborted == false`. It does not
automatically fail the enclosing Task. Successful outcomes contain a ResultSet
and no SQL Diagnostic. Transport, malformed protocol, cancellation, resource
limits and local admission failures still fail the Task/Result normally; a
partial or malformed exchange is not a valid retained SQL outcome.

Diagnostics retain their complete validated fields, including SQLSTATE,
severity, primary message, detail and hint when supplied. Copies survive later
queries, reset and connection destruction. Server detail/context may contain
sensitive application data: ownership is not sanitization or automatic logging.
No error ResultSet or result lifecycle event is fabricated for a SQL failure.
`Diagnostic::format` provides synchronous bounded verbosity/context formatting;
[policy, positions and ownership](postgres-diagnostics.md).

## Result Kinds

`ResultSet::kind` is assigned from wire transitions before lifecycle callbacks:

| Kind | Meaning |
| --- | --- |
| `uninitialized` | Default/application-built storage; no decoded server result |
| `empty_query` | Actual EmptyQueryResponse, not a SELECT with zero rows |
| `command` | Completed command without a row description, including COPY's terminal command |
| `tuples` | Completed described result, including zero rows or zero columns |
| `description` | Prepared/portal metadata without execution |
| `row_chunk` | Partial pipeline/standalone exchange rows, suspended fetch or a replication exchange boundary before completion |
| `acknowledgment` | Pipeline prepare/close acknowledgment without a synthetic command tag |

`result_kind_name(kind)` synchronously returns a nonallocating string_view of
the enum name, or `unknown` for an invalid value. Copies/moves preserve the kind;
mutating public result fields does not infer or recompute it. A moved-from
ResultSet is not a fresh server result.

COPY phases remain `CopyFormat`/data/CopyDone events. Pipeline Sync remains a
barrier with its existing `PipelineKind` and transaction snapshot, not an empty
query ResultSet. `PipelineResult::complete` still controls command admission;
`ResultSet::suspended` still tells a fetch caller whether more rows are pending.
Kinds do not replace those lifetime/phase contracts or libpq's entire C status enum.

## Status Names

`status_name(value)` is synchronous, noexcept and returns a string_view backed by
a static literal. Overloads accept ResultSet, Outcome, Diagnostic, CopyFormat,
ExchangeEvent, PipelineResult and std::error_code. They do not allocate, create
Tasks, consult a Connection, log payloads or invoke foreign error-category
callbacks. A returned name survives destruction of the inspected value.

```cpp
auto result = co_await weave::as_result(database.execute_outcome("SELECT 1 / 0"));
auto name = result
  ? weave::pg::status_name(*result)
  : weave::pg::status_name(result.error());
```

Use the existing typed kinds, variants, diagnostics and error codes for control
flow. Naming does not replace them with another status enum or change default
Task error propagation.

| Value | Name |
| --- | --- |
| ResultSet | Explicit `kind` name, including `description`, `row_chunk` and `acknowledgment` |
| Outcome with a result | That result's kind name |
| Outcome with an error Diagnostic | `sql_error` |
| Aborted Outcome | `aborted` |
| Nonempty standalone Diagnostic | `diagnostic`, without inferring notice/error origin from severity text |
| COPY format | `copy_input`, `copy_output` or `copy_both` |
| Exchange data, including an empty data frame | `copy_data` |
| Exchange CopyDone | `copy_done` |
| Acknowledged pipeline Sync | `pipeline_sync` |
| Pipeline command/chunk | Its Outcome name, with kind/completion-envelope checks |
| Zero error code | `success` |
| Encoded SQLSTATE error | `sql_error` |
| PostgreSQL protocol/authentication errors | `protocol_error`, `authentication_error`, `unsupported_authentication` |
| Other PostgreSQL errors | `resource_limit`, `closed`, `busy`, `unexpected_copy`, `target_session` |
| Standard generic/system cancellation condition | `canceled` |
| Other nonzero error codes | `error`; the original code retains its specific details |

Default/empty storage is `uninitialized`. Unrecognized enums or invalid encoded
SQLSTATE values are `unknown`. Conflicting known-enum Outcome/PipelineResult
envelopes are `invalid`: a result plus a diagnostic/abort, transaction metadata
on a non-Sync event, or a partial non-execution/non-chunk result, for example.
Completed pipeline prepare/close values require acknowledgments; description
values require descriptions. Unknown enum dispatch can take precedence over
these envelope checks.

This is inspection, not a replacement wire/text validator. Publicly constructed
rows, command tags, COPY formats and diagnostic fields are not revalidated, nor
used to guess a result kind. An Outcome's nonempty error slot identifies its
SQL-error envelope even if an application supplies malformed diagnostic fields.
A standalone Diagnostic retains no original frame-kind flag; an empty one is
`uninitialized`, and severity text does not manufacture that missing provenance.
Use the wire decoder and `Diagnostic::format` for their respective validation.

`copy_done` describes the receive-side marker, not successful completion of the
entire COPY exchange. `pipeline_sync` acknowledges a barrier even when its
transaction snapshot is `failed`; it does not mean rollback occurred. Likewise,
`success` means a zero error code, not that a server is healthy or a connection
is open. Naming a SQL error does not decide whether its connection remains usable.
Concurrent inspection requires immutable snapshots, not a concurrently mutated
Connection. Native C `PGRES_*` names are deliberately not reproduced: both
single-row and chunked partial data use `row_chunk`, and native command-success
values have more precise description/acknowledgment counterparts in Weave.

## Execution Contracts

Names, SQL and parameters are owned through lazy execution. The Connection borrow
is captured before initial suspension, so deferred outcome Tasks block reset and
conflicting session leases just like ordinary queries. ReadyForQuery drains
before an SQL outcome returns; transaction failure remains visible and requires
rollback. Cancellation or invalid pending responses are terminal and drain
native operations. Connections are not made thread-safe by retained data.

## Column Lookup

`ResultSet::column_index(identifier, encoding, limit)` is synchronous and returns
`Result<std::optional<std::size_t>>`. A present index is zero-based; an empty
optional means no match, not failure. Duplicate names return their first index.
Invalid syntax/encoding and resource limits are Result errors. No Context, Task,
connection access, row conversion or server work is involved.

```cpp
auto column = result.column_index("USER_ID");
if (!column)
  return std::unexpected(column.error());

if (*column) {
  auto index = **column;
  // Use index with the result's columns or rows.
}
```

Unquoted identifiers fold ASCII A-Z to a-z. They start with a letter, underscore
or encoded non-ASCII character; subsequent characters may also include digits
and dollar signs. Quoted identifiers preserve case and punctuation, with doubled
double quotes representing one quote. For example, `"\"DisplayName\""` selects
the exact name `DisplayName`; `"\"a\"\"b\""` selects `a"b`. An empty quoted
identifier can select an empty application-built column name.

Inputs must contain one complete identifier, not SQL, qualifications, whitespace
around a token or partial quoting. No trimming, implicit identifier-length
truncation or locale-dependent case folding occurs. Unlike libpq's permissive
parser, malformed partial/unclosed quoting is rejected; quoted spelling remains
available for arbitrary valid names. Non-ASCII characters and multibyte
continuations are preserved, not case-folded byte by byte.

The default encoding is UTF8, matching Weave's default connection policy. Supply
the actual encoding of the specific result for legacy text. ResultSet does not
capture an encoding tag or consult a live Connection; a later encoding change
does not reinterpret a retained result. `escape_identifier(raw_name, encoding)`
can produce the quoted lookup spelling for an owned column name. This lookup is
not an SQL escaping or application transcoding operation.

The default input bound is 65,536 bytes, including outer/doubled quotes. The
optional limit changes that bound explicitly. Excess input fails with
`Error::resource_limit`, without truncation. Normalization uses bounded local
storage and leaves result metadata unchanged. Concurrent reads of an immutable
ResultSet are supported; this does not permit simultaneous mutation or imply
that a Connection is thread-safe.

[libpq result reference](https://www.postgresql.org/docs/18/libpq-exec.html),
[qualification evidence](postgres-qualification.md) and the
[remaining API audit](postgres-api-audit.md) distinguish implemented capabilities
from full compatibility and deployment/release qualification.

## Selective Copies

`ResultSet::copy(ResultCopyOptions = {})` is synchronous and returns an owning
`ResultSet` directly, not a Task or Result wrapper. All three options default to
true; ordinary copy construction/assignment retain their existing behavior.

```cpp
auto schema = result.copy({.rows = false, .observers = false});
auto values = result.copy({.observers = false});
auto metadata = result.copy({
  .columns = false,
  .rows = false,
  .observers = false});
```

| Option | Effect |
| --- | --- |
| `columns` | Copy column definitions and their names/OID/format metadata |
| `rows` | Deep-copy row values, preserving binary bytes, empty values and NULL; implies columns |
| `observers` | Retain accepted lifecycle registrations and run their result-copy hooks |

Rows always bring their schema, even with `.columns = false`. To omit both,
set both `columns` and `rows` to false. Kind, command tag, parameter types and
portal-suspended state are always preserved, including a default-constructed
result's `uninitialized` kind. Copying does not validate or reinterpret
application-populated fields or change a kind based on the selected payload.

Selected containers own independent storage. Observer data follows the
[lifecycle hook's explicit clone/share policy](postgres-events.md), not an
implicit deep copy. With observers disabled the copy retains no registration,
callable or instance-data owner and emits no later lifecycle-destroy callbacks.
It can outlive the source and its callback owner independently.

Payload selection happens before copy callbacks. Their borrowed destination
is read-only and can be moved on return; do not retain its address. Hooks can
replace their instance-data slot, not the result payload. Copying a result during its
own lifecycle callback is a fatal contract violation even with observers disabled.
Immutable-source concurrent copies are supported; mutable results still require
external coordination. Allocation failure follows the ordinary exception-disabled
container policy, not a recoverable copy error.

[libpq's selective-copy API](https://www.postgresql.org/docs/18/libpq-misc.html#LIBPQ-PQCOPYRESULT)
also makes rows imply attributes, but always sets the destination status to
`PGRES_TUPLES_OK` and drops source error text. Weave preserves its actual kind
and metadata; SQL errors remain Task/Result failures or owning Outcome diagnostics,
not fabricated ResultSets. Notice handlers belong to the Connection, not results,
so no result notice-hook flag is exposed.

[Permanent qualification and native controls](postgres-qualification.md#selective-result-copy-qualification-2026-10-09).
