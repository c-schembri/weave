# PostgreSQL Diagnostics

`Diagnostic::format(DiagnosticFormat options = {})` synchronously returns an
owning `Result<std::string>`. It does not need a Context, await, connection,
native handle or logger. Raw `fields`, `field`, `message` and `sqlstate` remain
available; formatting never modifies the diagnostic.

```cpp
auto message = diagnostic.format({
  .verbosity = weave::pg::DiagnosticVerbosity::standard,
  .context = weave::pg::DiagnosticContext::errors,
  .query = query_text
});

if (!message)
  return std::unexpected(message.error());
```

Settings are explicit per invocation, not mutable Connection defaults. Notice
handlers can call the same formatter with application-owned policy. `query` is
an optional borrowed string_view used only during this synchronous call; Weave
does not silently retain SQL in a Diagnostic. Keep borrowed options/query text
alive through the call. The returned string owns any displayed text.

## Policy

Defaults are **terse** verbosity and **never** context. This avoids automatically
including detail, internal SQL and source metadata, but primary server messages
can still contain sensitive data. Formatting is neither sanitization nor secure
storage. Raw fields may include terminal controls; nothing is logged automatically.

| Verbosity | Output |
| --- | --- |
| `terse` | Severity, primary message and textual character position |
| `standard` | Primary message, available caret display, detail, hint, internal query and permitted context |
| `verbose` | Standard output plus SQLSTATE, schema/table/column/type/constraint names and source location |
| `sqlstate` | Severity and SQLSTATE; absent SQLSTATE falls back to terse |

`DiagnosticContext` controls the server `W` context field only: `never` omits
it, `always` includes it, and `errors` includes it for ERROR/FATAL/PANIC severity.
Classification uses nonlocalized `V`, with `S` as a fallback; an unknown severity
does not disclose context under `errors`. Unlike a native PGresult, Diagnostic
does not carry a separate fatal/nonfatal result-status flag. Terse/SQLSTATE
output never includes context, regardless of this setting. Context suppression
does not suppress other fields selected by standard/verbose verbosity.

Labels are fixed English; server field text is preserved, including localized
severity. Nonempty formatted messages include a trailing newline. Empty
Diagnostic storage returns an empty string, not a fabricated error message.
This is a server-diagnostic formatter, not a complete formatter for transport
error_codes or connection-error history.

## Positions And Encoding

Positions are positive, one-based logical character numbers, not byte offsets
or grapheme indices. Statement position `P` takes precedence over internal `p`.
For nonterse output, caller-supplied query text supports `P`; server internal
query `q` supports `p`. Without corresponding text, the position is appended
to the primary message. An explicitly empty query differs from absent text.
Positions past the supplied query's logical EOF do not fabricate a caret.

Caret displays count CR/LF/CRLF lines, replace tabs with one space and crop long
lines to a 60-column character-aligned window with visible ellipses. Widths use
the existing encoding utilities, treating nonpositive widths as one column for
this diagnostic display. UTF8 uses installed ICU data; this is not a claim of
identical native Unicode tables, terminal/grapheme layout or application transcoding.

The default encoding is UTF8. Supply the actual client encoding for legacy
diagnostics/query text. The Diagnostic owns field bytes, not an encoding tag or
a connection reference; later connection changes do not reinterpret them.

## Bounds And Failures

`input_bytes` defaults to 1 MiB across all field-value bytes and explicitly
supplied query bytes. `output_bytes` defaults to 16 KiB. There are at most 255
unique nonzero field codes; unknown extension codes remain acceptable. Bounds
are checked without arithmetic overflow, and cursor rendering does not allocate
a whole-query character/offset index.

Oversized input, output or field count returns `Error::resource_limit`, never
a silently partial formatted message. The marked caret window is intentional
display cropping, not truncation of diagnostic fields. All supplied text is
validated, including fields/query not selected for display. NUL or invalid
encoding returns `illegal_byte_sequence`; duplicate/zero codes, invalid enums,
malformed SQLSTATE and invalid/overflowing positions return `invalid_argument`.
SQLSTATE `00000` is valid for notices. Limits/options can be changed explicitly;
normal standard-library allocation failure retains the library's exception-disabled
allocation model, not a new recoverable-allocation guarantee.

Immutable diagnostics can be formatted concurrently. This does not permit
simultaneous mutation or make Connection thread-safe.

The [qualification record](postgres-qualification.md#bounded-diagnostic-formatting-2026-10-09)
covers native libpq 18.4/18.6 controls on Windows/Linux, all four verbosity modes,
three context policies, errors/notices, 42 encodings, lifetime/bounds and package
consumers. [Native formatting reference](https://www.postgresql.org/docs/18/libpq-exec.html)
and the [remaining API audit](postgres-api-audit.md) distinguish these mappings
from full compatibility and deployment readiness.

## Connection Reports

Pass an optional owning output to connect or reset when a single error code is
not enough:

```cpp
weave::pg::ConnectionReport report;
auto connection = ctx->run(weave::pg::connect(options, report));

if (!connection) {
  auto text = report.format();
  if (text)
    WEAVE_LOG_ERROR("%s", text->c_str());
  return weave::report_error(connection.error());
}
```

Async code uses `co_await as_result(pg::connect(options, report))` to inspect a
failure, or ordinary `co_await` to propagate it automatically. Matching
`BlockingConnection::connect(options, report)` and connection/blocking
`reset(fresh_options, report)` overloads use the same engine. Notice-handler and
Trace overloads remain available; reporting is not a logger or trace observer.

The report's `attempts` are **failed attempts only**, in execution order. Each
owns its configured host/port, optional attempted or actual TCP endpoint,
`ConnectionStage`, original error code and available server/provider Diagnostic.
Resolved-address failures are distinct entries; a successful host fallback keeps
earlier failures. Local sockets have no TCP endpoint. Stages distinguish
validation/admission, resolution, transport, socket tuning, peer identity, GSS,
TLS, startup, authentication, target-session checks and OAuth acquisition.

Each attempt also owns monotonic `elapsed` microseconds and a fixed `stage_times`
array indexed by `ConnectionStage`. `current` retains the latest attempt,
including a successful one; its stage/timing snapshot updates at phase changes
and completion, not continuously while I/O is pending. These are wall-clock
durations, not CPU time, spans or benchmarks. Failed-attempt formatting includes
the total elapsed time.

`error` is the final operation error, or zero on success. A deadline records
`timed_out` after its cancelled child drains, not a duplicate cancellation error.
Expected OAuth discovery rejection is part of a successful discovery exchange,
not a failed connection; acquisition and pinned-reconnect failures are recorded.
Authentication/security/protocol errors and cancellation still obey the existing
terminal policy. Reporting never permits another host or a weaker transport by
itself; only an explicit [allow/prefer TLS policy](postgres-connections.md#connection-strings)
authorizes its narrowly defined same-endpoint retry.

`completed` means the reporting operation finished, not that a reset failure
closed the original session. Invalid or busy resets can leave it usable. The
report is cleared when its Task body starts; dropping, rejecting or pre-cancelling
an unstarted Task leaves it untouched. Keep it alive and do not move, mutate,
share between operations or inspect concurrently while borrowed. Serialized
code on the executing Context may inspect the latest phase snapshot; there is
no cross-thread polling guarantee or callback API. After completion, copies
are independent of the Connection, Context and Task. An error before a physical
attempt can have a validation entry without an endpoint or host.

`authentication` retains the latest attempt's owning challenge/completion facts,
including successful startup and when history is truncated. Retained failed
attempts also own their facts. `authentication.complete` means validated
AuthenticationOK, whereas report `completed` means the factory/reset operation
has finished; neither implies success by itself. The existing formatter does not
automatically print these fields. [Authentication meanings and native flag limits](postgres-metadata.md#authentication-facts).

Capture retains at most `ConnectionReport::max_attempts` (4,096) entries and
`max_bytes` (1 MiB) of accounted entry/field storage. It is not an exact allocator
footprint or a bound on the connection's independent protocol buffers. If a
Diagnostic does not fit, the entry retains its host/stage/error, marks
`diagnostic_truncated`, and omits that Diagnostic. If an entry cannot fit, it is
omitted. Either sets report `truncated`; neither replaces the real operation error.
The separate latest-attempt snapshot is bounded independently and may duplicate
one retained diagnostic; it is not included in the failed-history byte counter.

`ConnectionReport::format` is synchronous and owning. It applies DiagnosticFormat
to server/provider fields, a global field/host/query input budget, and a global
output budget. It emits category/value/message for error codes and explicit
capture-limit markers. Hosts, category names and native messages are byte-escaped
when necessary; Diagnostic field disclosure/encoding rules are unchanged. Invalid
or oversized formatting returns an error, never a silently partial string.

Reports do not clone passwords, keys, bearer tokens, cancellation secrets,
Options or raw authentication packets. Server/provider messages can themselves
contain sensitive text: capture and formatting are **not sanitization**. No report
is logged automatically. The ordinary connection path has no report collector or
report-observer Task; opt-in reporting adds cold-path collector/observer storage,
not wrappers to query reads/writes.

This is connection/reset history. Use the session-failure snapshot below for
admitted post-login operation failures; neither API is a mutable per-invocation
libpq-formatted message buffer. The [API audit](postgres-api-audit.md) retains
that difference explicitly.

The [connection-report qualification](postgres-qualification.md#bounded-connection-reports-2026-10-09)
records tested profiles, bounded-capture controls, retained failures and evidence limits.

## Session Failures

`Connection::last_failure()` and `BlockingConnection::last_failure()` synchronously
return an owning `Result<Failure>` containing the primary local/SQL error code and
the retained server `Diagnostic`. Unlike `info()`, inspection is allowed after a
transport failure closes the session. A moved-from connection returns `Error::closed`.
An outstanding Task, pipeline/exchange lease, active operation or callback returns
`Error::busy`. Inspection does not make the connection thread-safe.

```cpp
auto query = co_await weave::as_result(database.query("SELECT 1"));
if (!query) {
  auto failure = database.last_failure();
  if (failure) {
    auto text = failure->format();
    if (text)
      WEAVE_LOG_ERROR("%s", text->c_str());
  }
}
```

`Failure::error` is zero when no failure has been retained. `format()` uses the
same explicit DiagnosticFormat disclosure, encoding and input/output bounds as
the other formatters. Native category/message text is byte-escaped. Empty failure
formats as empty text; malformed or oversized input returns an error, not partial
output. Raw diagnostic fields remain owning and available independently.

Admitted wire exchanges record propagated transport, protocol, resource and
cancellation failures during frame cleanup, before their promises die. Recovered
server SQL diagnostics remain inspectable even when an outcome-returning API
successfully returns an owning SQL-error outcome. A later call that merely finds
the failed transport closed does not replace the original cause. Existing new-
command diagnostic-clearing boundaries clear the corresponding local error;
successful reset creates fresh state, while failed replacement retains its error.

The diagnostic is the existing retained server/provider diagnostic, not a fabricated
explanation for every local error. A protocol/transport failure after a server error
can therefore contain both its local cause and the earlier server fields. There is
no new query/password/authentication-buffer retention, logging or per-read/write
reporting wrapper. Snapshots outlive moves, reset and destruction.

Pre-admission validation/busy errors, unstarted/rejected Tasks, callback/application
errors and deliberate closure without an observed operation failure are not new
wire failures. Inspect their original Task/Result errors directly. This is not a
log of every API invocation or a literal replacement for libpq's mutable message
buffer; the [API audit](postgres-api-audit.md) retains that difference explicitly.

Input limits cover diagnostic fields and the optional query; output limits cover
the complete formatted text. They do not bound allocation inside an application's
custom `std::error_category::message()` implementation.

The [session-failure qualification](postgres-qualification.md#owning-session-failures-2026-10-09)
records exact profiles, concurrency controls, retained defects and evidence limits.
