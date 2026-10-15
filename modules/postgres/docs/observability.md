# PostgreSQL Results And Observability

Inspection/registration/formatting is synchronous unless a method explicitly
waits for wire data. Owning snapshots do not make Connection thread-safe.
Keep session access serialized and drain deferred/active borrows before mutation.

## Results

ResultSet owns columns, rows, command tag, parameter types and suspended state.
Kinds distinguish uninitialized/application storage, empty query, command,
tuples, description, row_chunk and acknowledgment independently of row count.
COPY events and pipeline Sync are separate types, not fabricated ResultSets.
`status_name` overloads return static inspection labels; they do not replace
typed kinds, validate public payloads or decide whether a connection is reusable.

`query_outcomes`, `execute_outcome` and `execute_prepared_outcome` retain SQL
failures as owning Diagnostic values. Transport/protocol/cancellation failures
still fail the Task. Simple queries stop at first SQL error and do not invent
outcomes for unexecuted statements. `Outcome::failure` creates a checked owning
application diagnostic, not evidence received from the server or an error ResultSet.

`inserted_oid()` / `inserted_oid_text()` inspect INSERT command-tag OIDs, not a
modern table's generated primary key. Missing OID is zero/empty; malformed or
overflowing OIDs fail. Text views borrow the command tag.

`column_index(identifier, encoding, limit)` returns Result<optional<size_t>>.
Empty optional means no match; duplicates choose first. Unquoted names fold ASCII
case, quoted names preserve it. Require one complete identifier without trimming,
qualification or silent truncation. Default encoding is UTF8 and input bound 64 KiB.
Supply the encoding of that result, not a later connection setting.

### Primitive Access

`Row` remains an owning vector-like collection. Its selectors return borrowed
`Field` proxies: `row[index]`, `row["name"]` and `row[column_handle]`. All checked
conversions are synchronous `Result<T>` values, never awaitable or throwing
operational APIs. Selectors carry lookup/bounds errors until conversion; using
raw `bytes()`/`is_null()` inspection requires a valid selector. Iterate raw
Values normally, or use `row.values()` for explicit low-level storage mutation.
Direct indexed `.data` mutation must now use `row.values()[index].data`.

| Conversion | Result |
| --- | --- |
| `field.as<T>()`, `row.get<T>(index/name/handle)` | Checked primitive T |
| `field.integer<T>()` (default i64) | Signed/unsigned integer |
| `field.floating<T>()` (default double) | float/double |
| `field.boolean()` | bool |
| `field.text()`, `field.string()` | Borrowed string_view / owning string |
| `field.optional<T>()`, `field.as<std::optional<T>>()` | NULL-aware conversion |

Text numeric conversions require a complete representation, without whitespace,
trailing bytes or integer truncation. Numeric range errors fail; binary conversion
uses the source PostgreSQL OID/width, not the destination's width. Binary integers
support int2/int4/int8/OID; floating conversion supports float4/float8 and those
integer types. Floating conversion permits ordinary rounding and preserves NaN,
infinities and signed zero; overflow or underflow to zero fails. Text-to-floating
conversion of numeric values is approximate, not an exact-decimal decoder.
Booleans require `t`/`f` text or a one-byte 0/1 binary bool. Text views accept text
wire representations and binary char/name/text/bpchar/varchar, not arbitrary
binary numeric/bytea bodies. Use raw `bytes()` for undecoded wire bytes.

NULL fails for non-optional targets. For optional targets only SQL NULL becomes
nullopt; lookup and conversion errors remain failures. `DecodeError` distinguishes
null_value, invalid_value, out_of_range, unsupported_type, missing_column,
ambiguous_column and schema_mismatch. Primitive decoding itself does not allocate,
except when requesting owning strings; query/result storage still allocates.

Named selection is an exact, case-sensitive match of the returned column label;
duplicates fail rather than selecting the first. This differs from the existing
SQL-identifier-aware `column_index()` contract above. `result.column("name")`
returns a reusable ColumnHandle; indexed use avoids name searches and rejects
another schema's handle. The immutable wire schema survives retained/copied Rows
and ResultSet moves. A ResultSet deep copy gets its own schema identity. Editing
public column metadata does not rename/retype that wire snapshot; application-made
Rows without wire metadata support positional conversion only.

`auto fields = row.reader()` creates an independent borrowed cursor. Both
`fields.next().as<T>()` (including the primitive helpers) and `fields.next<T>()`
advance one field, even when its conversion fails. Reading past the end fails
without advancing. `remaining()` inspects, `reset()` rewinds and checked
`skip(count)` leaves the position unchanged on failure. Readers/Fields/views must
not outlive or overlap mutation of their owning Row storage. Row copies retain
their schema; readers are not stored as mutable state inside Rows.

### Struct Access

Include `<weave/postgres.hpp>` or `<weave/postgres/record.hpp>` for struct decoding.
`row.as<T>()` returns a synchronous `Result<T>`, matching column labels to member
names exactly. Column order is irrelevant and extra columns are ignored. Every
selected member uses the primitive conversion rules above; missing or ambiguous
columns still fail, even for optional members. No partially decoded struct is
returned and the source Row is never modified.

Automatic decoding uses bundled Boost.PFR headers pinned to Boost 1.89.0, with no
Boost installation or runtime dependency. Its compiler-specific name extraction is qualified on
MSVC and GCC in C++23 mode. Use named, externally linked simple aggregates with
public, assignable primitive/optional members; no bases, references, const/volatile fields,
bitfields, C arrays, nested records or user-declared constructors. The bundled
structured-binding backend supports up to 200 fields. Decoded types must be
default-constructible and movable.

`result.decoder<T>()` validates the mapping once and returns a `RowDecoder<T>`.
`decoder.decode(row)` uses cached indices, rejecting a foreign schema. It retains
schema metadata, not row data; it works with retained Rows and ResultSet moves,
but a deep-copied ResultSet needs its own decoder. Binding and primitive decoding
do not allocate; owned string members may allocate. String-view members borrow
the source Row and do not become safe to retain merely by moving the struct.

Aliases and non-aggregates can opt into explicit metadata; automatic decoding
needs none:

```cpp
struct Recipient {
  weave::i64 key;
  std::string name;
};

template <>
struct weave::pg::RowTraits<Recipient> {
  static constexpr auto fields = std::tuple{
    weave::pg::field("id", &Recipient::key),
    weave::pg::field("recipient", &Recipient::name)
  };
};
```

Explicit mappings select only the listed members; other members retain their
default initialization. They require a constexpr tuple of non-null member pointers
belonging to the target (including accessible base members), with writable
primitive/optional targets. Invalid mappings and unsupported member types are
diagnosed at compile time, including when binding a decoder without decoding a row.

### Copies And Storage

`copy(ResultCopyOptions)` returns ResultSet directly. Columns, rows and observers
default on; rows imply columns. Kind, command and scalar metadata are always
preserved. Selected payload storage is independent; observer data uses its explicit
clone/share policy. Observer-free copies retain no callbacks/data and emit no hooks.

ResultText/ResultList are owning adapters, **not exact std::string/std::vector or
ABI-compatible replacements**. They support ordinary views/mutation; standard
container conversions are explicit to avoid hidden owning temporaries at borrowed
coroutine boundaries. Keep an owning local alive through an await.

```cpp
std::string_view borrowed = result.command;
std::string owned{result.command};
auto prefix = result.command.substr(0, 6); // Independent std::string.
auto columns = static_cast<std::vector<weave::pg::Column>>(result.columns);
```

Text supports formatting/hash; substring and concatenation produce independent
std::string values, including embedded NUL bytes. Ordinary views retain source
lifetime requirements; an arena does not extend them. ResultSet is not an aggregate.

`memory_size()` reports reachable counted backing requests, including pool objects,
allocation headers/padding, container/proxy requests and result-local event records.
It is not capacity, allocator usable size, RSS or exclusive ownership across results.
Shared pools, including retained row-schema ownership records, count once within
each result; callback-owned application data and
inspection scratch are excluded. Chunks use separate row pools, but caller-retained
chunks remain application memory. Inspection may allocate scratch proportional to
pool identities; concurrent mutable inspection requires coordination. Logical
protocol limits are not physical allocator quotas. Unequal-allocator container
operations retain their allocator contracts; allocation failure is fatal under
the exception-disabled model, not a recoverable Result.

## Encoding

The 42 supported encodings provide validation/framing, not automatic transcoding.
PostgreSQL performs supported server conversions or returns SQL errors.
`set_client_encoding` sends a real SET and validates ParameterStatus; missing or
contradictory successful metadata is terminal. Invalid enum fails before I/O.

`parse_encoding` accepts bounded ASCII aliases; `encoding_info` returns static
metadata. `validate_text` checks the whole buffer; character-size/width helpers
inspect only the first character and return zero for empty input.
NUL, invalid/truncated sequences fail. Some legacy framing policies are permissive;
valid framing does not prove a codepoint is assigned or convertible.
UTF8 display width uses ICU, not grapheme/font/emoji layout or libpq's exact tables.

`escape_literal` / `escape_identifier` validate current encoding, preserve complete
multibyte boundaries and return fully quoted strings. Literal quoting uses E syntax.
They are deliberately stricter than permissive native escaping at ambiguous byte
boundaries. Keep encoding unchanged until executing generated SQL; prefer parameters.
`encode_bytea` emits hex text, not an SQL literal; `decode_bytea` also accepts
legacy octal/backslash forms. Both own output and enforce explicit size limits.

`ResultSet::format` supports bounded table, delimited and HTML display, including
headings, captions, NULL and row counts. Binary values render as hex; HTML escapes
markup. Delimited output escapes controls, so it is **not lossless CSV**.
Table widths use the selected encoding. No FILE/pager API or byte-exact libpq
display compatibility is promised.

## Diagnostics

`sqlstate(error)` decodes SQLSTATE; `last_error()` owns server fields.
`last_failure()` snapshots an admitted operation's local/SQL cause and retained
diagnostic, including after transport closure. Inspect only after operations
and leases drain; an old failure is not necessarily the current operation's outcome.

Optional caller-owned Diagnostic/ConnectionReport outputs on connect/reset must
outlive the lazy Task. Reports retain bounded actual host/address attempts,
latest failure, omissions and monotonic phase timings, not CPU time or a hard
whole-list deadline. Unstarted Tasks do not populate them. Selected configuration,
secrets and cancellation keys are not dumped into reports.

`Diagnostic::format(DiagnosticFormat)` returns bounded owning text synchronously.
Defaults are terse verbosity and no context. Standard/verbose expose detail/hint,
internal query and selected context; SQLSTATE mode is minimal. Context errors
classifies nonlocalized severity and does not disclose unknown-severity context.
Suppression is not sanitization: server messages/raw fields can contain secrets
or terminal controls. Nothing is automatically logged.

Positions are positive one-based logical characters, not bytes/graphemes.
Caller query supports statement positions; server internal query supports internal
positions. Supply actual encoding; snapshots have no encoding tag. Caret rendering
crops a character-aligned display window, not the owned fields.
Default input bound is 1 MiB and output 16 KiB. Every supplied field/query is
validated, even if hidden. Resource excess fails without partial output; duplicate
fields, bad SQLSTATE/options/positions, NUL and invalid encoding fail explicitly.

## Metadata

`info()` returns an owning selected-session snapshot: actual endpoint/local peer,
login/database, backend PID, protocol, server version, transaction, authentication
and negotiated TLS/GSS. It exports no secret/native handle and requires no
outstanding borrows. Startup user is not SQL current_role; configured options
are not a readback of current GUCs. Closed sessions fail inspection.

`Options::info()` inspects typed configuration without validation/provider calls.
`Options::defaults(ConfigSources)` explicitly performs synchronous source loading.
`Connection::configuration()` snapshots successful configuration before secrets
are discarded. Nonsecret application text still requires log policy.
Parameters distinguish absent from empty; version parsing is bounded and returns
unknown rather than fabricating a valid server version.

`transaction()` is synchronous: idle, active SQL transaction, failed, in_progress
unresolved protocol work, or unknown after closure. Constructing an unstarted
Task alone does not mean protocol progress; queued pipeline work does. An earlier
Sync cannot hide later debt. Pipeline status reflects the lease and actual abort
latch; only valid Sync clears it, not inspection or terminal closure.

Authentication facts distinguish recognized password request, credential presence
and validated AuthenticationOK. A challenge is not proof of sending a password;
AuthenticationOK is not authorization or completed ReadyForQuery. TLS metadata
distinguishes encryption, chain/name verification and authenticated peer identity.

## Notices And Notifications

Default notices/notifications use bounded owning queues, never automatic stderr.
`take_notices()` / `take_notifications()` drain them. Overflow fails rather than
silently dropping events. Optional owning noexcept handlers receive new events
inline; `on_notice` / `on_notification` synchronously return the previous callable
for save/restore. Empty handlers restore queue mode; queued events are not replayed.

Handlers can be supplied before startup. Replacement requires idle session
without deferred borrows; unpooled reset retains registrations. Pooled lease
return clears them. Callback arguments/views expire at return; copy intentionally
retained data. No suspension, nested event-loop driving or session reentry is allowed.
Callbacks must be prompt and synchronize shared captures across connections.

`wait_notification()` exclusively reads the idle session until an event arrives.
There is no background reader; do not issue SQL on the same session during a wait.
Installing a handler does not remove normal explicit-wait completion or change
LISTEN transaction semantics. Use a dedicated subscribed connection when needed.

## Lifecycle Events

`on_event(name, handler)` registers an owning noexcept Result-returning callable
with unique copied name and stable EventId. `EventData` is shared_ptr<void>;
application casts and clone/share policy must match the stored type.
Events cover registration, connection reset/destruction and result create/copy/
destruction. Borrowed object/source pointers are valid only during invocation.

Retained results own accepted registrations/data, not their Connection. Result
copy hooks explicitly select cloned/shared data; moves transfer without copy hooks.
Registration failure rejects the registration; result-create/copy failure suppresses
that observer on the destination without failing the query/value copy. Reset/destroy
errors cannot veto cleanup. Close/finish end transport, not logical Connection
destruction. Pool return ends checkout observer state.

Each registration serializes its callbacks, including result work on different
threads; different registrations are not mutually serialized. Never recursively
copy/destroy observed values or drive session loops from a callback. Avoid capture
cycles and borrowed pointers outliving owners. Immutable result copies can be
concurrent; mutable Connection/results still require coordination.

`event_data` / `set_event_data` are synchronous, with unknown-ID failure and empty
slot distinct from absent registration. `attach_events(ResultSet&)` attaches current
observers to populated application storage without SQL. Accepted IDs are skipped;
rejected IDs can be retried. It attempts remaining observers and returns the first
failure without rolling back accepted ones. Busy/deferred/nested dispatch is rejected.
It does not validate/fabricate server provenance or retain a pending registry.

## Protocol Tracing

`on_trace(Trace)` registers synchronously and returns previous configuration;
empty configuration disables it. A trailing connect argument observes startup.
Tracing defaults off; metadata mode withholds all payloads.
Explicit application mode returns bounded borrowed payloads during invocation.

Startup/pre-ReadyForQuery, auth and backend cancellation-key bodies are **always
withheld**, even in application mode. SQL, parameters, rows, notices and COPY
payloads can still disclose sensitive data; opt-in is not general sanitization.
The application owns logging, retention and access policy.

Callbacks run inline, without await or helper thread. Frontend events describe
attempted logical frames, not successful send/commit; backend framing can precede
semantic failure. Duplex traffic has no extra total-order guarantee.
Trace covers logical PostgreSQL frames, not TLS/GSS records, OAuth HTTPS or the
independent CancelHandle transport. Registration/callback reentry follows ordinary
idle/busy rules; copy payloads deliberately if retaining them.
