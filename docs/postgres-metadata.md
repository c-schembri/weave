# Connection And TLS Metadata

`Connection::info()` and `BlockingConnection::info()` are synchronous
`Result<ConnectionInfo>` queries. They perform no network I/O and return an
owning snapshot, not borrowed strings or native transport handles.

```cpp
auto info = database.info();
if (!info)
  co_await weave::fail(info.error());

WEAVE_LOG_INFO("Database: %s, user: %s, backend: %u",
  info->database.c_str(), info->user.c_str(), info->backend_process);

if (info->tls)
  WEAVE_LOG_INFO("TLS cipher: %s", info->tls->cipher.c_str());
```

The example belongs inside a Task with an idle `Connection`. In synchronous
code, inspect the `Result` directly; neither metadata API is awaitable.

## Session Fields

| Field | Meaning |
| --- | --- |
| `host`, `port` | Selected configured destination, after host selection; not the first unused entry in `Options.hosts` |
| `endpoint` | Actual connected TCP address and port; absent for local sockets |
| `local_address` | Actual local-socket peer name; absent for TCP |
| `peer_user` | Linux kernel peer UID when available; absent rather than fabricated on unsupported transports/platforms |
| `database` | Effective startup database, defaulting to the login user when no database was specified |
| `user` | Startup login user, not the current SQL role after `SET ROLE` |
| `server_options` | Effective selected `Options.server_options` text sent in Startup; not a server readback or current GUC snapshot |
| `server_version` | Server-reported version string; empty when the server has not supplied that parameter |
| `server_version_number` | Numeric reported version; zero when absent, unparseable, negative or outside the supported signed-32-bit range |
| `backend_process` | Backend process identifier, not the cancellation secret |
| `protocol_version` | Negotiated protocol version |
| `authentication` | Owning AuthenticationInfo: selected method, password-challenge/presence facts and validated AuthenticationOK completion |
| `transaction` | Current known transaction state, or `in_progress` while protocol work remains unresolved |
| `gss_encrypted` | Whether the actual transport uses GSS encryption, independently of the PostgreSQL authentication method |
| `tls` | Negotiated TLS snapshot; absent on plaintext, local or GSS-encrypted transport |

Snapshots survive connection moves, reset, finish and destruction. Reset produces
a new session; previously returned values retain the old session's information.
Modifying a snapshot does not alter the live connection.

Inspection requires serialized access with no outstanding session borrowers.
Deferred Tasks already borrow the Connection; an outstanding Task,
pipeline/exchange lease or active operation
causes `Error::busy`. Closed and moved-from connections return `Error::closed`.
A metadata failure does not close an otherwise usable session.

A row/COPY phase between individual operations does not by itself forbid
inspection. Its transaction field reports `in_progress` until the terminal
ReadyForQuery is consumed, consistently with the scalar getter below.

These checks do not make Connection thread-safe. Do not race inspection with
reset, move, destruction or unrelated execution owners. The Connection and its
Context must retain their normal lifetimes.

## Transaction State

`Connection::transaction()` and `BlockingConnection::transaction()` return
`Transaction` synchronously, without network I/O or a Task/Result wrapper:

```cpp
auto transaction = database.transaction();
```

| Value | Meaning |
| --- | --- |
| `idle` | Open session, no SQL transaction and no unresolved protocol work |
| `active` | Open SQL transaction; this does not mean a command is executing |
| `failed` | Failed SQL transaction, as reported by ReadyForQuery |
| `in_progress` | Pending protocol work makes the previous ReadyForQuery state provisional |
| `unknown` | Closed or moved-from session, including terminal cancellation, protocol/transport failure and failed reset |

The scalar getter permits serialized query callbacks and row/COPY/pipeline
intervals; owning `info()` retains its stricter borrower/callback admission checks.
Neither getter makes the Connection thread-safe. Do not inspect from an unrelated
root/thread or race reset, move or destruction.

Constructing an unstarted query/reset Task does not mark progress. Metadata
inspection, an empty pipeline lease and a passive notification wait do not
represent a new command. Pipeline admission does mark progress, including queued
unsent work. Receiving an earlier Sync must not hide later command/Sync debt.
After the final acknowledged Sync, buffered application results alone do not
keep the connection in progress. A Sync event's transaction snapshot describes
that particular barrier even when the Connection still has later work.

These are Weave states, not numeric aliases for libpq enums. In the pinned
libpq 18.4/18.6 controls, consuming an unsynchronized pipeline command and its
null result exposes the last transaction state before Sync. Weave deliberately
keeps `in_progress` until synchronization establishes a current state; it does
not claim libpq's polling/result-consumption lifecycle. Compare the
[native status contract](https://www.postgresql.org/docs/18/libpq-status.html).

Permanent controls cover nine Windows/Linux build profiles, both four-worker
schedulers, Windows shared IOCP, malformed peers, active-operation cancellation,
real row/COPY streaming over verified mTLS and relocated packaging.
[Exact evidence and limits](postgres-qualification.md#transaction-progress-and-unknown-state-2026-10-09).

## Configuration Snapshots

Three synchronous APIs return owning `OptionsInfo` values:

- `Options::info()` inspects typed settings without validation, environment reads,
  filesystem access or provider invocation. An invalid Options object can still
  be inspected; the snapshot is not evidence that connection setup will succeed.
- `Options::defaults(ConfigSources)` explicitly resolves the same sources as
  `Options::load`, returns `Result<OptionsInfo>` and cleanses temporary owned
  credentials. It can perform synchronous file/LDAP/identity lookups, just like
  the explicit loader; it is not a hidden lookup inside an async connection.
- `Connection::configuration()` and `BlockingConnection::configuration()` return
  `Result<OptionsInfo>` for the configuration captured before successful startup
  discards credentials and candidate-only state.

```cpp
auto configuration = database.configuration();
if (!configuration)
  co_await weave::fail(configuration.error());

auto retained = std::move(*configuration);
```

Configuration means requested/resolved settings, not actual negotiated transport
or current SQL settings. The complete configured host list stays in `hosts`,
including order and numeric addresses; `ConnectionInfo` separately identifies
the selected host and actual endpoint. An empty database resolves to the login
user. Later ParameterStatus messages do not rewrite the initial configuration.

The whitelist includes identity, startup settings, host selection, socket policy,
timeouts, resource limits, authentication/protocol policy and nonsecret TLS/OAuth
options. It excludes passwords, per-host passwords, SCRAM keys, private-key
passphrases, OAuth secrets/tokens, callbacks and provider/native handles.
`tls_context`, `gss_context` and `oauth->provider` record only supplied-object
presence, not validity, ownership or negotiated use. Prebuilt provider internals
are opaque. When neither plaintext nor a prebuilt TLS context is selected,
missing `tls_options` expands to the default client policy in the snapshot.

Loaded Options carry an optional `origin` with the source policy and selected
service/service-file/password-file paths. A password-file path does not prove
the file was opened or supplied a password. This is a historical loader record,
not per-field provenance; subsequent caller edits to Options do not reconstruct
it. Arbitrary application strings and paths are not sanitized or automatically
logged.

Session inspection rejects outstanding Tasks, leases and callbacks with
`Error::busy`, without retiring the transport. Unlike `info()`, the stored
configuration remains readable after finish or transport closure. Moved-from
connections return `Error::closed`. Returned copies are independent historical
values; they do not make Connection thread-safe. Successful reset installs a
new snapshot rather than changing old copies.

Failed reset leaves the previous configuration available on the closed connection;
successful recovery installs the fresh configuration and clears the failure ledger.
Inspection does not change retained error history. Actual ParameterStatus changes,
such as a later client-encoding change, remain separate from configured intent.

Permanent controls passed nine Windows/Linux profiles, including Debug, Release,
ASan, no-Runtime builds and Linux with GSS/LDAP disabled. Coverage includes both
four-worker schedulers, Windows shared IOCP, failed reset/transport retention,
real file-configured mTLS/encrypted keys and relocated packaging/header checks.
This qualifies the documented snapshot contract, not every libpq keyword, provider
internals, arbitrary deployment or the full PostgreSQL release.
[Evidence and limits](postgres-qualification.md#configuration-snapshot-regression-qualification-2026-10-09).

## Parameters And Versions

Both connection facades expose synchronous
`Result<std::optional<std::string>> parameter(std::string_view name)`:

```cpp
auto setting = database.parameter("TimeZone");
if (!setting)
  co_await weave::fail(setting.error());

std::optional<std::string> retained = std::move(*setting);
```

The outer Result reports inspection failure. A successful `nullopt` means the
server has not reported that parameter; an engaged empty string means it reported
an explicitly empty value. An engaged nonempty string contains the reported
value. The value is owning: later ParameterStatus messages, reset or destruction
do not mutate previously returned copies. This replaces the previous string-only
return type and is a source-breaking change, not an additional alias.

The same idle-session, closed and busy rules as `info()` apply. Embedded NUL in
the name returns `invalid_argument` without retiring the session. Inspection
does not read pending network messages or issue SQL. Unlike libpq's borrowed
cached pointer, Weave requires serialization with outstanding Tasks and leases;
neither metadata API is awaitable.

Numeric conversion follows libpq's decimal-prefix rules: `18.4` becomes `180004`,
`9.6.2` becomes `90602`, and `19devel` becomes `190000`. Three-component input
uses the legacy three-component calculation even with a modern major version;
for example `18.4.1` becomes `180401`. Partial numeric prefixes and vendor suffixes
retain the native interpretation. These rules come from the
[libpq status contract](https://www.postgresql.org/docs/18/libpq-status.html) and
[native ParameterStatus conversion](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/interfaces/libpq/fe-exec.c#L1094).
Negative or overflowing numeric input deliberately yields zero rather than
reproducing unsafe native integer conversion/arithmetic. The raw reported string
is retained, and malformed version text does not by itself close the connection.
Version parsing fixtures do not qualify connections to obsolete server releases.

`server_options` records the effective request text, matching the purpose of
[PQoptions](https://www.postgresql.org/docs/18/libpq-status.html). It is not a
complete connection-options schema and does not report later SQL configuration
changes. Arbitrary application-supplied option text can contain sensitive values;
the snapshot is not sanitized and must not be automatically logged. This field
does not clone Options, passwords, provider objects or other credential storage.

## Pipeline State

`Connection::pipeline_status()` and `BlockingConnection::pipeline_status()`
return `PipelineStatus` synchronously and `noexcept`, without a Task or I/O:

| Value | Meaning |
| --- | --- |
| `off` | No owning pipeline lease; also moved-from connections |
| `on` | A lease exists without an observed abort-until-Sync latch |
| `aborted` | A lease exists and a server error has latched abort-until-Sync |

Creating an empty pipeline reports `on`; queueing alone cannot discover a server
error. Parsing an acknowledged Sync clears the latch, even if the application
has not consumed older error/skipped results. Queueing a Sync does not clear it.
Successful `finish()` releases the lease immediately, even while the finished
Pipeline object remains alive. Dropping unsent work also releases it without
sending those commands.

This is neither transport health nor SQL transaction state. Cancellation, EOF or
malformed traffic can close the transport while its pipeline lease remains `on`
or `aborted`. Inspect `open()` separately. After protocol recovery, a pipeline
can be `on` while `transaction()` is `failed`; the SQL transaction still needs
ROLLBACK. Destroying the unfinished pipeline releases its lease and reports `off`.

Inspect only on the serialized connection graph, including its synchronous
notice/trace callbacks. Internal locking does not permit unrelated roots or
threads to race connection operations, move or destruction. A trace callback
runs before that frame's semantic parsing, so it observes the prior latch.
`ConnectionInfo` deliberately has no pipeline-status field: its owning snapshot
requires no outstanding pipeline lease, making such a field always `off`.

This maps the useful three-state [libpq pipeline-status
capability](https://www.postgresql.org/docs/18/libpq-pipeline-mode.html), not its
result-consumption or exit lifecycle. Weave requires acknowledged synchronization
before `finish`; native libpq 18.4/18.6 controls allow exit after consuming all
unsynchronized command results. Weave also treats malformed backend traffic as
terminal rather than preserving libpq's connection-health flag for an unknown
framed tag. See [pipeline ownership](postgres-pipelines.md#ownership-and-limits).
Permanent nine-profile regression, real-server and packaging gates cover this
[documented scope](postgres-qualification.md#connection-pipeline-status-2026-10-09).

## Authentication Facts

`ConnectionInfo::authentication`, `ConnectionAttempt::authentication` and
`ConnectionReport::authentication` contain the same scalar `AuthenticationInfo`:

| Field | Meaning |
| --- | --- |
| `method` | Selected known authentication method; none with incomplete authentication does not mean the server accepted trust authentication |
| `password_requested` | A structurally valid password/MD5 challenge or recognized SCRAM offer was observed for the selected exchange; not proof that a password was sent or accepted |
| `password_missing` | A password challenge was observed and the selected effective password was empty, captured before credential cleanup |
| `complete` | AuthenticationOK was accepted after the existing proof, channel-binding and provider checks; not ReadyForQuery, connection health or application authorization |

The method is now `info->authentication.method`; comparing the former
`info->authentication` enum directly is a source-breaking API change. No legacy
comparison alias is added. These facts own no password, token, SCRAM key,
provider object or borrowed connection pointer.

Successful sessions expose facts through the synchronous checked `info()`
snapshot. For a failed factory/reset, pass a ConnectionReport and recover with
`as_result` as in the [report example](postgres-diagnostics.md#connection-reports).
The top-level report records the latest attempt, including successful startup
and cases where the bounded failed-attempt history fills. Each retained failed
attempt keeps its own facts. Copies survive move, reset, finish and destruction.
Report borrowing and completion rules remain unchanged; do not inspect it while
the Task is running.

Authentication can complete before a subsequent startup error. Conversely,
validation/transport/TLS failure may occur before any challenge, leaving default
facts. Malformed or unsupported challenge payloads do not fabricate a valid
password request. Policy rejection is still terminal; observing a request does
not permit another host, weaker transport or automatic credential retry.

Despite its name, native `PQconnectionUsedPassword` reports the password-demand
flag, including failed attempts before a password response. `PQconnectionNeedsPassword`
combines that flag with absence of a configured password. See the
[status contract](https://www.postgresql.org/docs/18/libpq-status.html) and
[native implementations](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/interfaces/libpq/fe-connect.c#L7244).
Weave calls these facts `password_requested` and `password_missing` rather than
claiming actual credential use. A password-free successful SCRAM key login can
have both flags true; neither is by itself a reason to prompt or retry. The
primary error and configured security policy remain authoritative. Native partial
flags on malformed SASL input are not a compatibility guarantee.

## TLS Fields

Standalone `TlsStream<S>::info()` returns the same owning `TlsInfo` used by
`ConnectionInfo::tls`. It records the implementation name (`OpenSSL`), actual
negotiated TLS version, cipher name, symmetric cipher `key_bits`, compression,
ALPN selection and actual `session_reused` outcome. `key_bits` is not the
certificate's public-key size. Empty ALPN means no application protocol was
selected. Server session configuration alone does not imply resumption occurred.

`peer` is present only when the handshake authenticated a peer certificate.
Clients authenticate the server; mTLS servers authenticate a supplied client
certificate. A server permitting an absent client certificate has no peer
identity in that case. Each `TlsPeerIdentity` owns the subject, issuer, SHA-256
fingerprint and DER-encoded leaf `certificate`. This is not the complete chain,
a private key, a session ticket or an OpenSSL pointer. Exported leaf size is
bounded by the credential snapshot's certificate-chain limit.

TLS inspection serializes native engine access, but does not permit concurrent
move/destruction or relax the stream's transport-affinity rules. Before verified
handshake completion it fails; after a sticky TLS failure it returns that error.
Moving from a stream makes its `info()` return `TlsError::closed`.

A retained certificate is historical evidence of the completed handshake, not
fresh revocation verification or an application authorization decision. The
application still decides which authenticated identities may perform actions.

## Compatibility And Qualification

The snapshot maps useful [libpq connection status and TLS
attributes](https://www.postgresql.org/docs/18/libpq-status.html) to owning C++
values. It is not a literal replacement for every status function: retained
passwords, cancellation keys and native SSL/socket/GSS handles are deliberately
not exported. Owning configuration snapshots are now implemented; their final
qualification and complete connection-keyword/provider introspection remain open.
SQL-side `current_user` can differ from the startup user.

Permanent controls cover endpoint selection, independent copies, reset/move and
closed/busy boundaries, transaction-state updates, blocking operation, verified
mTLS and four-worker runtimes with both schedulers. TLS engine controls cover
both supported versions, all client-certificate policies, fresh and resumed
handshakes, exact DER certificates and snapshot lifetime. See the
[frozen qualification records](postgres-qualification.md#metadata-regression-qualification-2026-10-08).
This is affected-feature qualification, not complete libpq parity or an
independent security audit.

Parameter presence, effective startup options and numeric version conversion
have a separate [qualification record](postgres-qualification.md#session-parameter-and-version-metadata-2026-10-09):
24 version cases across context/blocking and available four-worker policies,
19 defined cases matched against isolated native libpq controls, reset/move and
busy boundaries, nine build profiles, real PostgreSQL sessions and relocated
consumers. Malformed negative/overflow controls are explicitly not native-parity
claims.

Authentication facts have their own
[qualification record](postgres-qualification.md#authentication-fact-snapshots-2026-10-09),
including failed factories, reset, password-free key login, cancellation,
multicore policies, native flag controls, real sessions and package consumers.
