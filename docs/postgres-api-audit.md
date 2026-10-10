# libpq 18 API Audit

This is a function-by-function inventory, not a percentage-complete claim.
The target remains libpq's capabilities expressed through Weave's owning,
exception-free APIs. Marking a C function as replaced does not prove every
associated capability or deployment is qualified.

The 2026-10-08 inventory uses the installed Windows libpq 18.4 and Linux libpq
18.6 public headers. They are byte-identical: `libpq-fe.h` declares 187 functions,
including 16 large-object and three encoding functions; `libpq-events.h` adds
six, for **193 distinct functions**. SHA-256:

- `libpq-fe.h`: `499d984421f5490be016f7d49a8599e7f8be120f0cf80c200e872265064be7c0`
- `libpq-events.h`: `1f68ab7cf5e957bac8e9a4994e6dc6253736b231ddfa1ae4ea61517a583f202a`

Each function has exactly one table row. The three compatibility macros are
listed separately. This inventories callable interfaces, not every enum,
feature macro, connection keyword, server version or operating-system behavior.
The [capability matrix](postgres-parity.md), detailed guides and
[qualification record](postgres-qualification.md) remain necessary evidence.

## Reading The Status

- **Mapped:** a concrete Weave API supplies the capability; signatures and error
  representation can differ. This does not assert literal C compatibility.
- **Partial:** some useful behavior exists, but a named semantic or ergonomic
  gap remains. It is not counted as full parity.
- **Open:** no corresponding capability is implemented yet.
- **Different:** an intentional ownership/security/execution-model difference;
  the consequence is stated rather than hidden as an implementation win.
- **External:** behavior specific to libpq, its ABI or its native integration;
  Weave has no matching object to expose. This is not a general portability claim.

In the tables, `Connection` also means the matching `BlockingConnection`
operation when present. Async operations return `Task<T>`; the blocking facade
drives the same engine and returns `Result<T>`. Synchronous inspection/queueing
does not become asynchronous merely because its owner is a Connection.

## Connect And Configuration

Weave mappings are in [connection.hpp](../modules/postgres/include/weave/postgres/connection.hpp),
[blocking.hpp](../modules/postgres/include/weave/postgres/blocking.hpp) and
[configuration.hpp](../modules/postgres/include/weave/postgres/configuration.hpp).
[Connection-control reference](https://www.postgresql.org/docs/18/libpq-connect.html).

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQconnectStart` | Different | Lazy `pg::connect(Options)` Task; parse/load are explicit, no independently pollable startup object |
| `PQconnectStartParams` | Different | Typed `Options` and lazy connect; keyword/URI expansion is not the C array API |
| `PQconnectPoll` | Different | Await connect through Context/Runtime; no startup readiness states or caller-driven native polling |
| `PQconnectdb` | Mapped | `BlockingConnection::connect`; explicit `Options::parse/load` chooses ambient configuration behavior |
| `PQconnectdbParams` | Mapped | Typed Options and blocking connect; not literal keyword-array/expand-dbname processing |
| `PQsetdbLogin` | Mapped | Typed host/port/user/database/password/server_options fields and blocking connect |
| `PQfinish` | Mapped | Owning destruction or `close`; `finish` also awaits protocol termination, and borrowers must drain |
| `PQconndefaults` | Partial | Static option_schema and qualified owning Options::defaults expose metadata and explicit resolved nonsecret values; full keyword/deployment coverage remains open; strict loader errors/security defaults differ |
| `PQconninfoParse` | Partial | Owning `Options::parse`, with documented supported/rejected keywords; not every libpq option |
| `PQconninfo` | Partial | Owning configuration captures nonsecret typed startup settings and configured hosts; provider internals remain opaque, secrets are not exported, and full keyword/final deployment qualification remains open |
| `PQconninfoFree` | Different | Options own their storage and use normal C++ destruction |
| `PQresetStart` | Different | Lazy `reset(fresh_options)` Task; credentials are not retained for implicit reuse |
| `PQresetPoll` | Different | Await reset; no exposed native polling state machine |
| `PQreset` | Mapped | Blocking reset with explicit fresh Options and optional startup Diagnostic |

## Cancellation

[Connection contracts](postgres-connections.md#cancellation) and
[cancel_handle implementation](../modules/postgres/src/connection.cpp) cover
endpoint/security snapshots and protocol-version-specific secrets.
[Cancellation reference](https://www.postgresql.org/docs/18/libpq-cancel.html).

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQcancelCreate` | Mapped | `Connection::cancel_handle()` captures an owning immutable backend/security snapshot |
| `PQcancelStart` | Mapped | `CancelHandle::request()` returns an independent lazy Task |
| `PQcancelBlocking` | Mapped | `CancelHandle::request_blocking()` outside an executing Context |
| `PQcancelPoll` | Different | Await the request; native connect/send/drain states are not exposed |
| `PQcancelStatus` | Different | Request Task/Result completion; no mutable cancellation-connection status object |
| `PQcancelSocket` | Different | No native descriptor export; Context owns completion handling |
| `PQcancelErrorMessage` | Mapped | Request error_code; no libpq-formatted borrowed error buffer |
| `PQcancelReset` | Different | Construct another request from the immutable handle; capture a fresh handle after query-session reset |
| `PQcancelFinish` | Different | RAII handle/Task ownership; active request completion still must drain |
| `PQgetCancel` | Mapped | Same owning cancel_handle capability without a legacy separate representation |
| `PQfreeCancel` | Different | Ordinary handle destruction |
| `PQcancel` | Partial | Blocking cancellation exists, but is not async-signal-safe; do not call Weave from a signal handler |
| `PQrequestCancel` | Mapped | `Connection::request_cancel()` captures before suspension; no deprecated shared-session mutation |

## Connection Status And Security

[Metadata contracts](postgres-metadata.md), [GSS contracts](postgres-gss.md) and
[encoding contracts](postgres-encoding.md) describe the actual public boundaries.
[Status reference](https://www.postgresql.org/docs/18/libpq-status.html).

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQdb` | Mapped | Owning `info().database` effective startup database |
| `PQuser` | Mapped | Owning `info().user` login identity, not later SQL SET ROLE |
| `PQpass` | Different | Selected authentication secrets are cleared after startup, not retained/exported |
| `PQhost` | Mapped | Owning `info().host` selected configured host |
| `PQhostaddr` | Mapped | Typed actual TCP `info().endpoint`; absent for local sockets |
| `PQport` | Mapped | Selected numeric `info().port`; no borrowed string |
| `PQtty` | External | Obsolete libpq compatibility accessor; no Weave tty connection concept |
| `PQoptions` | Mapped | Owning `info().server_options` effective Startup request text; not current GUCs, complete option introspection or sanitized application text |
| `PQstatus` | Partial | Connect Result and `open`; no startup phases, failed PGconn analogue or proactive remote health check |
| `PQtransactionStatus` | Mapped | Synchronous transaction reports idle/open-transaction/failed/in_progress/unknown on async and blocking connections; queued/unsynchronized pipeline work stays in_progress until final Sync, deliberately differing from libpq's result-consumption lifecycle |
| `PQparameterStatus` | Mapped | Synchronous Result of owning optional string distinguishes absent from explicit empty; checked idle-session access, not a borrowed mutable cached pointer |
| `PQprotocolVersion` | Mapped | Typed `protocol_version`; not libpq's major-only integer representation |
| `PQfullProtocolVersion` | Mapped | Typed negotiated v30/v32 wire value; not libpq's decimal major/minor encoding |
| `PQserverVersion` | Mapped | Owning `info().server_version_number` uses native decimal-prefix/version formulas; raw string retained, malformed negative/overflow inputs safely yield zero |
| `PQerrorMessage` | Partial | Owning last_failure retains admitted-operation local/SQL errors and server diagnostics after transport closure; bounded ConnectionReport formats connect/reset attempts; no per-invocation mutable libpq-style message buffer |
| `PQsocket` | Different | No native socket export or manual readiness integration |
| `PQbackendPID` | Mapped | `backend_process` getter or owning info snapshot |
| `PQpipelineStatus` | Mapped | Synchronous Connection/BlockingConnection pipeline_status reports off/on/aborted lease and recovery state; not transport health, native result-consumption timing or unsynchronized exit policy; [contract](postgres-metadata.md#pipeline-state) |
| `PQconnectionNeedsPassword` | Mapped | Owning report/attempt authentication.password_missing records a recognized challenge with no effective password, without a failed live Connection or retained secrets; not an automatic retry/prompt instruction |
| `PQconnectionUsedPassword` | Mapped | authentication.password_requested maps password-demand facts on success/failure; native UsedPassword is not proof of actual password use, including SCRAM key passthrough; malformed native partial flags are not promised |
| `PQconnectionUsedGSSAPI` | Mapped | Negotiated authentication method; independent of GSS transport encryption |
| `PQclientEncoding` | Mapped | Typed synchronous client_encoding Result |
| `PQsetClientEncoding` | Mapped | Typed setter confirms server-reported encoding before success |
| `PQsslInUse` | Mapped | Presence of `info().tls` identifies actual TLS transport |
| `PQsslStruct` | Different | No mutable native SSL export; owning TLS attributes/authenticated peer DER instead |
| `PQsslAttribute` | Mapped | Typed TlsInfo library/version/cipher/key_bits/compression/ALPN fields |
| `PQsslAttributeNames` | Different | Fixed typed fields, not a runtime string-key schema or null-connection library probe |
| `PQgetssl` | Different | No OpenSSL handle export |
| `PQinitSSL` | External | No libpq initialization state; OpenSSL 3 engine setup is library-managed |
| `PQinitOpenSSL` | External | No libpq initialization state or caller-provided libpq initialization flags |
| `PQgssEncInUse` | Mapped | `gss_encrypted` reflects actual protected transport |
| `PQgetgssctx` | Different | No raw native GSS context export; provider identity/diagnostic APIs are owning |

## Diagnostics, Notices And Tracing

Mappings: [Diagnostic/notice API](postgres-notices.md),
[protocol trace API](postgres-tracing.md), and
[control reference](https://www.postgresql.org/docs/18/libpq-control.html).

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQsetErrorVerbosity` | Different | Explicit per-call DiagnosticFormat.verbosity supplies all four modes; no mutable connection formatting default or previous-policy return |
| `PQsetErrorContextVisibility` | Different | Explicit per-call DiagnosticFormat.context supplies never/errors/always; errors uses nonlocalized severity rather than PGresult status; no mutable connection default |
| `PQsetNoticeReceiver` | Mapped | Owning noexcept on_notice callback, replacement/save/restore and reset retention |
| `PQsetNoticeProcessor` | Mapped | Owning notice handlers compose with synchronous Diagnostic::format for owning formatted text and explicit policy; formatter failure is a checked Result, not a fabricated diagnostic string |
| `PQregisterThreadLock` | External | No libpq internal lock registration; native provider interoperability still needs its own qualification |
| `PQtrace` | Mapped | Owning on_trace observer with metadata default and explicit bounded application-body disclosure |
| `PQuntrace` | Mapped | Empty Trace replacement disables observation and returns prior registration |
| `PQsetTraceFlags` | Different | Typed TraceOptions, not libpq FILE formatting/timestamp/regression flags; authentication/key bodies always withheld |

## Commands And Streaming Results

Mappings: [query guide](postgres.md), [pipeline guide](postgres-pipelines.md),
[exchange guide](postgres-exchanges.md),
[execution reference](https://www.postgresql.org/docs/18/libpq-exec.html) and
[async reference](https://www.postgresql.org/docs/18/libpq-async.html).

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQexec` | Mapped | Blocking query returns all successful result sets, not only libpq's final result; SQL failures propagate |
| `PQexecParams` | Mapped | Blocking execute with owned nullable text/binary parameters, OIDs and result format |
| `PQprepare` | Mapped | Blocking prepare with parameter OIDs; success is void rather than synthetic command result |
| `PQexecPrepared` | Mapped | Blocking execute_prepared with owned parameter values and format |
| `PQsendQuery` | Different | Lazy query/start_rows/exchange Tasks; no standalone write-only ordinary-command acceptance API |
| `PQsendQueryParams` | Mapped | Async execute or synchronous Pipeline::execute queueing with explicit progress |
| `PQsendPrepare` | Mapped | Async prepare or Pipeline::prepare queueing |
| `PQsendQueryPrepared` | Mapped | Async execute_prepared or Pipeline::execute_prepared queueing |
| `PQsetSingleRowMode` | Mapped | start_rows/read_row; pipeline RowOptions.chunk_rows = 1 for extended-query delivery |
| `PQsetChunkedRowsMode` | Mapped | Extended pipelines and standalone exchange(sql, RowOptions) yield owning bounded chunks; standalone async/blocking paths have nine-profile permanent/native/real-server/reduced-module/packaging controls; no post-submission setter, native status enum or identical command-tag placement |
| `PQgetResult` | Mapped | Await query, Exchange::next, Pipeline::next or read_row; owning typed events replace PGresult/null cycles |
| `PQisBusy` | Different | Task completion and busy admission errors, not a pollable ordinary-session progress flag |
| `PQconsumeInput` | Different | Await operations drive input; no arbitrary nonblocking consume-only call |
| `PQenterPipelineMode` | Mapped | Synchronous pipeline() returns an owning exclusive session lease |
| `PQexitPipelineMode` | Mapped | Pipeline::finish requires synchronization, consumed results and drained borrowing Tasks |
| `PQpipelineSync` | Mapped | Queue sync then drive flush/send and consume its correlated barrier; queueing alone is not dispatch |
| `PQsendFlushRequest` | Mapped | Synchronous Pipeline/BlockingPipeline::request_flush queues a bounded marker without I/O, ID or result; nine-profile permanent/native/real-server/mTLS/reduced-module/packaging controls pass; drive transport explicitly using send/receive/flush, not libpq polling |
| `PQsendPipelineSync` | Mapped | Synchronous Pipeline::sync queueing without implicit transport flush |
| `PQnotifies` | Mapped | Owning take_notifications, explicit wait_notification or consuming on_notification observer |

## COPY, Progress And Fast Path

[COPY reference](https://www.postgresql.org/docs/18/libpq-copy.html),
[raw replication scope](postgres-replication.md) and
[mixed result/COPY API](postgres-exchanges.md).

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQputCopyData` | Mapped | Await write_copy/Exchange::write; borrowed input survives actual transport completion |
| `PQputCopyEnd` | Mapped | end_copy(error) or Exchange::finish_send(error); finish_copy_send provides COPY BOTH send-half closure |
| `PQgetCopyData` | Mapped | Owning optional chunk from read_copy or Exchange::next; await rather than async flag/zero status |
| `PQgetline` | Different | Modern chunk API; legacy line splitting and terminator handling are not reproduced |
| `PQputline` | Different | Modern write_copy with explicit byte span |
| `PQgetlineAsync` | Different | Modern owning awaited chunks; no legacy line/readiness interface |
| `PQputnbytes` | Mapped | write_copy accepts an explicit byte span |
| `PQendcopy` | Mapped | end_copy drains command/ReadyForQuery; owning terminal results retained |
| `PQsetnonblocking` | Different | Choose Connection Tasks or BlockingConnection; no mode mutation on a shared session |
| `PQisnonblocking` | Different | API type identifies execution model; no mutable mode bit |
| `PQisthreadsafe` | External | No libpq global predicate; Connection remains single execution owner, not a shared thread-safe session |
| `PQping` | Mapped | ping/ping_blocking with explicit Options parsing/loading; recovering-host, wire/TLS, real-server and Linux protected-GSS controls qualified across Windows/Linux profiles; broader domain/deployment qualification remains separate |
| `PQpingParams` | Mapped | Typed Options probe distinguishes accepting/rejecting/no_response; configuration/security/cancellation remain Task/Result errors; strict protocol validation and startup-EOF retry deliberately differ from native libpq; [scope](postgres-connections.md#server-availability) |
| `PQflush` | Different | Await writes or Pipeline send/duplex flush; no ordinary-session partial-write polling status |
| `PQfn` | Mapped | call_function with owned nullable values and text/binary formats; SQL failures preserve SQLSTATE |

## Result Inspection

Storage is [ResultSet, Column, Value and Outcome](../modules/postgres/include/weave/postgres/connection.hpp).
An explicit failure need not be an error ResultSet, and all successful results
retain their own data. Do not infer a result kind from row count alone: an empty
SELECT can have columns and no rows; a description is not query completion.

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQresultStatus` | Mapped | Protocol-derived ResultSet.kind, Outcome diagnostics/aborted, and typed COPY/pipeline events; no fabricated error ResultSet |
| `PQresStatus` | Mapped | Nonallocating status_name overloads span result/outcome, diagnostic, COPY, pipeline and error-code values; typed Weave names, not PGRES aliases or fabricated error ResultSets |
| `PQresultErrorMessage` | Mapped | Owning query_outcomes/execute_outcome/execute_prepared_outcome diagnostics, batch/pipeline errors and last_error snapshots |
| `PQresultVerboseErrorMessage` | Mapped | Diagnostic::format supplies bounded owning text, verbosity/context/encoding and optional explicit query; stricter input validation, safe defaults and fixed-English labels documented |
| `PQresultErrorField` | Mapped | Diagnostic::field with owning Diagnostic lifetime; SQLSTATE also encoded in error_code |
| `PQntuples` | Mapped | ResultSet.rows.size() |
| `PQnfields` | Mapped | ResultSet.columns.size() |
| `PQbinaryTuples` | Mapped | Inspect Column/Value Format; no legacy aggregate flag |
| `PQfname` | Mapped | Column.name |
| `PQfnumber` | Mapped | Synchronous column_index returns a checked optional first index; explicit encoding, strict complete identifiers, ASCII-only folding and bounded normalization; malformed partial quoting and locale-dependent byte folding deliberately differ |
| `PQftable` | Mapped | Column.table OID |
| `PQftablecol` | Mapped | Column.attribute |
| `PQfformat` | Mapped | Column.format |
| `PQftype` | Mapped | Column.type OID |
| `PQfsize` | Mapped | Column.type_size |
| `PQfmod` | Mapped | Column.modifier |
| `PQcmdStatus` | Mapped | Owning ResultSet.command |
| `PQoidStatus` | Mapped | Checked borrowed inserted_oid_text() preserves INSERT OID spelling; empty for non-INSERT |
| `PQoidValue` | Mapped | Checked synchronous inserted_oid() rejects malformed or overflowing tags; zero for non-INSERT |
| `PQcmdTuples` | Mapped | affected_rows gives checked numeric count instead of borrowed decimal text |
| `PQgetvalue` | Mapped | Value.bytes/data; explicit text/binary handling |
| `PQgetlength` | Mapped | Value.bytes().size(); distinguish NULL through is_null |
| `PQgetisnull` | Mapped | Value::is_null with nullable owned storage |
| `PQnparams` | Mapped | Description ResultSet.parameter_types.size() |
| `PQparamtype` | Mapped | Description parameter_types[index] OID |

## Prepared Statements And Portals

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQdescribePrepared` | Mapped | Connection::describe, including parameter/column metadata |
| `PQdescribePortal` | Mapped | Connection/BlockingConnection::describe_portal returns owning metadata without executing the portal |
| `PQsendDescribePrepared` | Mapped | Async describe or Pipeline::describe queueing |
| `PQsendDescribePortal` | Mapped | Awaited Connection::describe_portal or Pipeline::describe_portal queueing |
| `PQclosePrepared` | Mapped | close_prepared; success is void rather than synthetic result |
| `PQclosePortal` | Mapped | close_portal; success is void rather than synthetic result |
| `PQsendClosePrepared` | Mapped | Async close_prepared or pipeline queueing |
| `PQsendClosePortal` | Mapped | Async close_portal or pipeline queueing |

## Result Ownership And Construction

[Lifecycle event contracts](postgres-events.md) describe observer behavior on
actual query results, copies and application-built values.

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQclear` | Different | Owning ResultSet destruction; results can outlive the Connection |
| `PQfreemem` | Different | Standard owning values/deleters instead of public C allocations |
| `PQmakeEmptyPGresult` | Different | Application ResultSet construction and checked Outcome::failure(Diagnostic) own their data; no fabricated native wire status or automatic pending result-observer lifecycle |
| `PQcopyResult` | Mapped | Synchronous ResultSet::copy selects columns/rows/observers; rows imply schema, hooks choose clone/share and can reject their destination registration; Weave preserves actual kind/metadata instead of forcing TUPLES_OK, and notice handlers remain Connection-owned; [contracts](postgres-results.md#selective-copies) |
| `PQsetResultAttrs` | Mapped | Application-owned columns can be populated directly |
| `PQresultAlloc` | Different | Application-owned C++ allocations; no public result arena allocator |
| `PQresultMemorySize` | Mapped | Synchronous memory_size reports distinct reachable retained backing requests; nine-profile permanent regression, eight native controls, real-server, reduced-build and packaging gates pass; shared pools are nonexclusive, inspection may allocate scratch, and opaque receiver/application allocations are excluded; not RSS or libpq byte identity; [boundary](postgres-result-storage.md) |
| `PQsetvalue` | Mapped | Application-owned Row/Value storage can be populated directly |

## Escaping And Display

[Encoding and quoting contracts](postgres-encoding.md) require actual client
encoding and deliberately reject unsafe NUL/continuation-quote cases.

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQescapeStringConn` | Different | Complete quoted escape_literal, not unquoted fragments dependent on standard_conforming_strings |
| `PQescapeLiteral` | Mapped | Encoding-aware Connection::escape_literal |
| `PQescapeIdentifier` | Mapped | Encoding-aware Connection::escape_identifier |
| `PQescapeByteaConn` | Different | encode_bytea produces hex value text, not an SQL-escaped literal; bind it or quote separately |
| `PQunescapeBytea` | Mapped | Bounded owning decode_bytea accepts hex and legacy bytea representations |
| `PQescapeString` | Different | Standalone escape_literal requires explicit/default UTF8 encoding; no ambient mutable encoding |
| `PQescapeBytea` | Different | Bounded owning encode_bytea; no legacy C SQL-fragment allocation |
| `PQprint` | Different | Bounded owning ResultSet::format supports table/delimited/HTML; no FILE handles, pager or byte-exact legacy rendering |
| `PQdisplayTuples` | Different | Owning table/expanded display; encoding-width policy and safe control rendering differ |
| `PQprintTuples` | Different | Owning configurable headings/separators/NULL/count display; no native output stream mutation |

## Large Objects

[Public APIs](../modules/postgres/include/weave/postgres/large_object.hpp),
[ownership and client-file guide](postgres-large-objects.md) and
[large-object reference](https://www.postgresql.org/docs/18/lo-interfaces.html).
Both async and blocking overloads exist except client-file operations, which
are deliberately blocking. Descriptors remain transaction/session-bound.

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `lo_open` | Mapped | lo::open with typed Access |
| `lo_close` | Mapped | lo::close |
| `lo_read` | Mapped | Owning chunk or caller-owned writable span overload |
| `lo_write` | Mapped | lo::write returns count; write_all handles the complete span |
| `lo_lseek` | Mapped | Checked 64-bit lo::seek supports narrower positions too |
| `lo_lseek64` | Mapped | lo::seek with i64 offset/position and typed Seek |
| `lo_creat` | Mapped | lo::create without requested OID; no obsolete creation-mode argument |
| `lo_create` | Mapped | lo::create with optional requested OID |
| `lo_tell` | Mapped | lo::tell returns i64; caller can check a narrower conversion |
| `lo_tell64` | Mapped | lo::tell |
| `lo_truncate` | Mapped | lo::truncate with checked nonnegative i64 size |
| `lo_truncate64` | Mapped | lo::truncate |
| `lo_unlink` | Mapped | lo::remove |
| `lo_import` | Mapped | Blocking lo::import_file |
| `lo_import_with_oid` | Mapped | Blocking import_file with requested OID |
| `lo_export` | Mapped | Blocking lo::export_file |

## Utilities And Authentication Hooks

[Encoding API](../modules/postgres/include/weave/postgres/encoding.hpp),
[password API](postgres-passwords.md), [OAuth provider API](postgres-oauth.md)
and [TLS credentials](tls.md).
[Miscellaneous reference](https://www.postgresql.org/docs/18/libpq-misc.html) and
[OAuth reference](https://www.postgresql.org/docs/18/libpq-oauth.html).

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQlibVersion` | External | Weave does not wrap/link libpq and has no libpq runtime version to report |
| `PQsocketPoll` | External | No native libpq descriptor; Weave uses IOCP/io_uring rather than a caller polling helper |
| `PQgetCurrentTimeUSec` | External | std::chrono supplies time; no libpq socket-poll deadline format |
| `PQmblen` | Mapped | Bounded character_size with typed encoding; invalid/truncated inputs are Result errors |
| `PQmblenBounded` | Mapped | character_size takes an explicit bounded string_view |
| `PQdsplen` | Partial | character_width exists; UTF8 uses installed ICU rather than identical libpq Unicode tables |
| `PQenv2encoding` | Different | Explicit Options::load/parse_encoding; no hidden environment lookup in encoding helpers |
| `PQencryptPassword` | Mapped | Free password_verifier with explicitly selected legacy MD5 |
| `PQencryptPasswordConn` | Mapped | Connection/blocking password_verifier; explicit or server policy, MD5 needs opt-in |
| `PQchangePassword` | Mapped | Connection/blocking change_password; cleansed owned input and trace payload suppression |
| `PQsetAuthDataHook` | Different | Per-provider owning OAuth callbacks and prompts, not mutable process-global hooks |
| `PQgetAuthDataHook` | Different | Application retains its provider; no process-global registration getter |
| `PQdefaultAuthDataHook` | Partial | Native HTTPS/device provider exists; not every libpq default hook/deployment behavior is qualified |
| `pg_char_to_encoding` | Mapped | parse_encoding returns typed supported encoding |
| `pg_encoding_to_char` | Mapped | encoding_info supplies canonical name |
| `pg_valid_server_encoding_id` | Mapped | encoding_info.server distinguishes frontend-only encodings |
| `PQgetSSLKeyPassHook_OpenSSL` | Different | No global native password-hook registration; explicit owned TLS credential options |
| `PQsetSSLKeyPassHook_OpenSSL` | Different | Owned per-credential TlsPasswordProvider handles PEM/DER/configured STORE loading; no process-global hook, PGconn callback argument, legacy ENGINE setup or stdin fallback; [contracts](tls.md#server-and-adapters) |
| `PQdefaultSSLKeyPassHook_OpenSSL` | Different | Credential factory never prompts on stdin; applications supply credentials explicitly |

## Lifecycle Events

[Owning lifecycle API](postgres-events.md) maps real result lifetimes rather
than manufacturing PGresult objects for unrelated errors, rows and COPY events.
[Event-system reference](https://www.postgresql.org/docs/18/libpq-events.html).

| libpq function | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQregisterEventProc` | Mapped | on_event with unique name, owning noexcept handler and stable EventId |
| `PQsetInstanceData` | Mapped | Connection::set_event_data with owning application state |
| `PQinstanceData` | Mapped | Connection::event_data |
| `PQresultSetInstanceData` | Mapped | ResultSet::set_event_data |
| `PQresultInstanceData` | Mapped | ResultSet::event_data |
| `PQfireResultCreateEvents` | Mapped | Synchronous Connection/BlockingConnection::attach_events invokes current registrations on populated application results, skips accepted IDs and retries rejected hooks; no two-stage pending registry; nine-profile native/regression/real-server/packaging controls passed |

## Compatibility Macros

| libpq macro | Status | Weave mapping or remaining difference |
| --- | --- | --- |
| `PQsetdb` | Mapped | Typed Options and BlockingConnection::connect |
| `PQfreeNotify` | Different | Owning Notification destruction |
| `PQnoPasswordSupplied` | Different | No dependency on libpq's deprecated diagnostic string; structured missing-password reporting remains open |

Enum/status constants are not callable functions. Format, authentication,
transaction, COPY direction, pipeline kind/outcome and typed errors cover many
uses; the partial rows above explicitly retain missing states. Feature-test
macros and trace/result-copy flags are not reproduced as compatibility aliases.

## Capability Boundaries

The current inventory is **130 Mapped, eight Partial, zero Open, 47 Different
and eight External** (193 functions). These are API mappings, not a completion
percentage or a claim that every deployment is qualified.

Remaining Partial rows describe explicit model boundaries: configuration does not
silently read ambient state; cancellation is not an async-signal-safe native C
operation; connection status and error snapshots are not libpq's mutable polling
objects; Unicode display width and OAuth callbacks use different contracts. Read
the individual rows rather than treating a broad category as drop-in parity.

INSERT-OID extraction, bounded result display, application-created failures,
hashed CRL directories and PEM/DER/configured OpenSSL STORE keys are implemented.
STORE does not reproduce legacy ENGINE installation and redacts secret-bearing
locators. [Result contracts](postgres-results.md) and [TLS contracts](tls.md)
document ownership, security and output differences.

The fixed implementation batch is complete. Windows domain/HSM/provider/FIPS
deployments and independent security review are external qualification boundaries,
not evidence supplied by this audit. Existing replica, OAuth and LDAP guides retain
their precise deployment limits. [Latest validation](postgres-closure.md).

The subsequent migration batch adds all six TLS modes, explicit conventional
credential loading, latest-attempt phase timings and opt-in sensitive key logging.
These expand configuration behavior without inventing more libpq function aliases.
Strong defaults, restricted weak-mode fallback and deployment boundaries remain;
[contracts](postgres-connections.md#explicit-libpq-migration-profile).
