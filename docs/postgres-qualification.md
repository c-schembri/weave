# PostgreSQL Qualification

**Current checkpoint:** [fixed-scope implementation, validation and measurements](postgres-closure.md).
**Migration additions:** [TLS modes, explicit loading, phase timings and key logging](postgres-migration-qualification.md).
The dated records below are historical evidence. Their pending notes describe
those source snapshots and are superseded where the current checkpoint supplies
explicit results. They are not an instruction to add more scope.

This is an implementation checkpoint, not a production-readiness claim or full
libpq parity. The [feature matrix](postgres-parity.md) remains the release scope.
Security review, long deployment soaks, complete replication workflows and
several authentication and compatibility capabilities remain open.

Latest direct-TLS checkpoint: [permanent server and GSS-priority controls](#direct-tls-server-regression-qualification-2026-10-10).
Latest SNI checkpoint: [public stream adapter regressions](#sni-stream-adapter-regressions-2026-10-10).
Latest certificate-policy checkpoint: [permanent Windows regressions and packaging](#client-certificate-policy-regressions-2026-10-10).
Whole-module qualification, broader deployments and final matched measurements remain separate gates.

## Mixed Exchange Checkpoint, 7 October 2026

`Exchange`/`BlockingExchange` preserve mixed simple-query results and input,
output or bidirectional COPY phases as owning wire-order events. Session and
deferred-method leases prevent early reuse/destruction. Replication-only result
boundaries without individual CommandComplete are preserved, without inventing
command tags. Structurally valid keepalives following server CopyDone are
tolerated without automatic acknowledgement; arbitrary late payloads are rejected.

Real-server exploration also found an existing COPY lifetime bug: a deferred
write from one COPY could send into a later COPY. Reads, writes and completion
Tasks now capture a phase generation synchronously and reject stale phases.
Regression cases exercise old writes, reads, end_copy and finish_copy_send.
Send-half closure is published before awaiting native completion, so an earlier
peer response cannot observe a falsely open application-send direction.

The independent Python peer constructs its own packets without libpq or Weave's
wire helpers. Its 26 standalone cases cover COPY formats/counts, result ordering,
empty payload versus CopyDone, SQL-error recovery versus deferred-sender terminal
failure, limits, partial EOF, cancellation/draining, unfinished abandonment and
bidirectional socket backpressure. Runtime adds 16 unpinned roots per four-worker
scheduler/layout combination. Windows covers sharded/shared IOCP; Linux covers
its supported sharded layout. These are correctness tests, not measurements.

Private PostgreSQL 18.6 clusters qualify mixed COPY IN/OUT, moved exchanges and
deferred leases, blocking operations, BASE_BACKUP metadata/archive/manifest
transport, and logical insert/commit payloads through both COPY APIs. Built-in
pgoutput is mandatory; the optional contrib test_decoding path runs when its
library is installed and otherwise reports its absence explicitly. Multicore
cases run 16 concurrent mixed-query clients and logical streams under both
schedulers and all supported layouts, with verified mTLS. No data is restored,
replayed or implicitly acknowledged as durable.

| Gate | Windows | Linux / WSL |
| --- | --- | --- |
| Full non-packaging Debug suite | 24/24 | 28/28 |
| Full non-packaging Release suite | 24/24 | 28/28 |
| Full non-packaging ASan suite | 24/24 | 28/28 |
| Complete independent exchange fixture with Runtime | 30 cases | 28 cases |
| Supplemental complete ASan exchange fixture repetitions | 5/5 | 5/5 |
| Relocated component packaging and standalone headers | 9/9 | 9/9 |
| PostgreSQL-only controlled and real exchange gates, without Runtime | Passed | Passed |
| Native Windows real exchange Debug/Release/ASan gates | 3/3 | Covered by full suites |

Raw diagnostics, source/executable fingerprints and exploratory sources remain
in ignored evidence; ad hoc source/targets are removed after archiving. Earlier
benchmark tables retain their older frozen-source provenance. This checkpoint
does not imply a security audit, exhaustive fuzzing, complete replication
workflows or full PostgreSQL release qualification.

## Cancellation Checkpoint, 7 October 2026

The independent cancellation backend exposed a real library bug: a lazy
`Connection::request_cancel()` retained `this`, then selected the replacement
backend after reset. It now captures `Result<CancelHandle>` synchronously and
passes that owning snapshot into the existing request coroutine. There is no
borrowed Connection pointer or additional session lease; cancellation remains
independent of ordinary exchanges. Closed-source errors are captured too.

The permanent Python backend independently packs and checks complete protocol
3.0/3.2 cancellation requests. Its matrix covers plaintext and verified mTLS,
old versus replacement backend keys, deferred requests after Connection move,
destruction and original Context destruction, destroyed handles/credential
owners, closed-source errors, nonempty response rejection, TLS refusal/invalid
replies/expired certificates, SSL-reply/handshake/EOF deadlines, inherited
cancellation and already-cancelled admission with zero native submissions.
Expected failure paths check exact error categories and completion draining.
Fatal-contract subprocesses cover moved-from handles and blocking requests
inside an executing Context.

One/four-worker Runtime cases send 16 concurrent requests per combination of
protocol, transport and scheduler, using unpinned roots. Windows covers both
IOCP layouts; Linux covers its supported sharded layout. The blocking case uses
four caller threads with four independent requests each. A successful dispatch
does not prove a particular SQL statement was interrupted; existing real-server
gates separately observe executing statements and inspect their eventual results.

| Gate | Windows | Linux / WSL |
| --- | --- | --- |
| Full non-packaging Debug suite | 23/23 | 26/26 |
| Full non-packaging Release suite | 23/23 | 26/26 |
| Full non-packaging ASan suite | 23/23 | 26/26 |
| Cases per complete cancellation fixture with Runtime | 100 | 84 |
| Supplemental complete ASan cancellation fixture repetitions | 5/5 | 5/5 |
| PostgreSQL-only cancellation cases, without Runtime | 68/68 | 68/68 |
| Native Windows real-server Debug/Release/ASan gates | 3/3 | Covered by full suites |

The fixture waits for the client's half-close before closing its independent
backend socket. Its initial unread TLS close_notify caused Windows resets;
that was a fixture error, not a reason to weaken transport semantics. Both that
diagnostic and the reproduced deferred-request library failure are retained in
ignored evidence. This checkpoint does not claim exhaustive race coverage,
production readiness or new benchmark results; existing timing tables retain
their older frozen-source provenance.
Ad hoc sources and targets are removed after archiving diagnostic evidence;
source and executable fingerprints remain in ignored outputs, not Git.

## Local Transport Checkpoint, 7 October 2026

PostgreSQL now uses the native Local transport for explicit filesystem socket
directories on Windows/Linux and abstract directories on Linux. TLS policy is
never silently discarded: local connections require explicit plaintext. Numeric
Linux UID requirements are checked before startup; retained cancellation handles
reconnect to the actual path and recheck the original peer UID before secrets.
That earlier gate covered numeric UID policy only. Subsequent explicit Linux
username loading is qualified below; Windows peer credentials remain unsupported.

| Gate | Windows | Linux / WSL |
| --- | --- | --- |
| Full non-packaging Debug suite | 22/22 | 25/25 |
| Full non-packaging Release suite | 22/22 | 25/25 |
| Full non-packaging ASan suite | 22/22 | 25/25 |
| Isolated PostgreSQL/Local/TCP and combined relocated packaging | 4/4 | 4/4 |
| Supplemental PostgreSQL ASan process repetitions | 20/20 | 20/20 |

Permanent controlled backends cover protocol 3.0/3.2, unavailable-host traversal,
no startup bytes after UID rejection, no failover after identity failure, retained
cancellation handles, complete version-specific cancellation packets, and explicit
pending-startup cancellation with completion draining. Four-worker tests run 16
concurrent roots per combination of scheduler and successful-completion mode;
Windows covers both IOCP layouts. The roots are unpinned, not `spawn_on` controls.
Parser/loader tests cover owned URI/keyword paths, bounds, drive/IPv6 disambiguation,
ambiguous hostaddr rejection, and exact-directory password-file matching.

The disposable Linux PostgreSQL 18.6 runner now qualifies both filesystem and
abstract sockets through Task and blocking APIs, including pipelines, streaming
COPY/rows, fast-path functions, large objects, cancellation and reset. It also
runs 16 real local sessions under each four-worker scheduler. Linux's extra
abstract gate is opt-in with the same real-server setting, not a hosted benchmark.
Native Windows real-server Task/blocking and multicore gates passed separately
in Debug/Release/ASan through the direct WSL interface; those TCP/mTLS runs do not
certify a native Windows PostgreSQL server's Unix-socket listener.

Before permanent tests were added, an independent Python backend also exercised
a retained cancellation handle whose filesystem socket was replaced by a Linux
process with a different UID. The replacement received EOF with **zero secret
bytes**. That supplemental check required an isolated privileged test process;
ordinary CI does not imply coverage of privileged UID changes. A separate private
abstract-only PostgreSQL cluster accepted both libpq and Weave clients; this was
interoperability verification, not a new performance measurement.

Exploratory failures were in scratch fixtures: GCC's coroutine initializer-list
internal error, an incorrect extra `end_copy()` after COPY OUT had already drained,
and a missing include followed by an invalid stale-probe invocation. They were
retained as diagnostics rather than attributed to the library or counted as passes.
Final gates passed. Ad hoc source/targets are removed; raw qualification evidence
and source/executable fingerprints are ignored, not committed. Existing benchmark
tables refer to their own older frozen sources and do not measure this integration.

## Concurrent Benchmark Checkpoint, 7 October 2026

The [matched Runtime/libpq comparison](postgres-concurrency.md) completed four
sequential plaintext/TLS sweeps on frozen source: 840 measured windows and 186
retained pilot/calibration windows, with matching consumed results and no failed
measurements. Thirty-two simultaneous sessions use 1/2/4 client workers on native
Windows, and 1/2 in WSL after a fixed two-server-core reservation. Four Linux
workers are explicitly unavailable, not oversubscribed. Both Weave schedulers
are compared with native nonblocking libpq loops, not thread-per-connection code.

The optional comparison target and Python driver passed Debug, Release and ASan
builds on both platforms, five PostgreSQL CTest entries per Windows configuration
and six per Linux configuration, including real-server qualification. Forty-eight
short concurrent checks matched results in Debug and ASan on both platforms.
PostgreSQL-only and all-component packaging passed on both platforms. Eight
synthetic tooling tests run without a database or native timing measurements;
performance remains outside correctness CI.

Exploratory setup/test failures are retained and attributed in ignored evidence:
an older system libpq selection, a local benchmark-disabled build, Python 3.14
metadata interacting with a subprocess mock, and stripped WSL regex quoting.
Corrected final gates passed. No timing outliers or failed measurements were
discarded or selectively rerun. Source/executable hashes and a source snapshot
are recorded separately from the older serial baseline.

Throughput and P99 are qualified independently; noisy metrics remain inconclusive.
Windows process CPU time demonstrably underreports sustained worker activity and
cannot rank CPU utilization. Raw process cycles are reported without conversion
to time. Linux reports process CPU time. The Windows database path includes WSL
localhost forwarding, and VM-to-host CPU placement is unknown. Neither those
limits nor near-parity throughput establish universal performance, maximum server
capacity, full libpq compatibility or production readiness.

## Authentication and Protocol Checkpoint, 7 October 2026

Authentication restrictions and protocol-version bounds passed 19 sequential
build/test/packaging gate groups. All 42 PostgreSQL source/test/build files were
hashed before qualification and unchanged afterward:

- Windows Debug, Release and ASan passed four PostgreSQL CTest entries each;
  Linux Debug, Release and ASan passed five each, including a disposable real
  PostgreSQL 18.6 primary and standby.
- Native Windows real-server qualification passed separately in all three
  configurations. Both four-worker schedulers mix protocol 3.0 and 3.2 among
  64 concurrent plaintext/mTLS sessions each.
- The independent Python backend passed 31 authentication/negotiation cases,
  including independently calculated MD5/cleartext responses, no response after
  policy rejection, repeated/switched challenges, malformed/downgraded protocol
  negotiation and version-specific cancellation-key bounds. A separate fallback
  listener verifies security/protocol failures do not attempt another host.
- The independent SCRAM verifier passed 13 cases, including allow/exclude policy,
  required handshake completion, Unicode normalization and TLS channel binding.
  Numeric fixture addresses avoid unrelated IPv6 connection-refusal retries;
  dedicated DNS/IPv6 traversal coverage remains in the connection tests.
- Real-server SCRAM-PLUS, explicitly enabled MD5 and verified-TLS password
  authentication passed both protocol versions through Task and blocking APIs.
  Weak-method defaults, denied methods and wrong passwords were rejected.
  Parser and explicit-loader gates cover typed validation, service/environment
  precedence and rejection before network submission.
- Windows ASan passed five repetitions of three protocol/authentication entries:
  15 executions. Linux ASan passed five repetitions of all five PostgreSQL entries:
  25 executions, including real-server and concurrent runtime qualification.
- PostgreSQL-only and all-component packaging passed on both platforms, including
  relocated consumers and standalone public headers.

An exploratory Windows build overlapped a running test executable and failed to
link. Initial real-server harness checks also attempted to run the blocking
facade inside a Task, which both Context backends correctly rejected. Final gates
were serialized, and blocking checks now execute outside the coroutine driver.
These exploratory failures are recorded separately from frozen-source passes.

Ad hoc sources and CMake targets were removed. Windows deletion of ignored probe
artifacts and its aborted certificate fixture was blocked by execution policy;
the remaining paths are recorded in ignored raw evidence. The attributable Linux
crash fixture was removed. No benchmark measurements were rerun. This checkpoint
does not establish full libpq parity, an independent security audit, or a long
deployment soak.

## Host Balancing Checkpoint, 7 October 2026

Opt-in random host traversal and per-host DNS-address shuffling passed 18 fresh
build/test/packaging gate groups on frozen source:

- PostgreSQL selections passed Windows Debug, Release and ASan (three CTest
  entries each), and Linux Debug, Release and ASan (four entries each).
- Native Windows real-server qualification passed separately in all three
  configurations. Both four-worker schedulers mix ordered and random traversal
  among their 64 concurrent plaintext/mTLS probes.
- Async and blocking primary/standby selection, prefer-standby fallback and reset
  passed. Typed per-host passwords override an intentionally wrong global secret.
  Service/environment parsing retains precedence and configured destination order.
- Native localhost IPv4/IPv6 fixtures cover the random single-host address path.
  Rejection/deadline fixtures verify exactly one security/cancellation attempt,
  both eligible timed-out hosts, and stopping after successful startup. Tests
  deliberately do not require a lucky distribution or every possible permutation.
- Linux ASan passed five repetitions of all four PostgreSQL entries: 20 passes
  in 54.31 seconds, including real-server and both scheduler profiles.
- PostgreSQL-only and all-component packaging passed two entries on each platform,
  including isolated builds, relocated consumers and standalone public headers.

Qualification cancellation now waits for evidence of an executing statement,
not a fixed sleep: async probes observe a server notice, and the blocking
cross-thread probe uses a separate same-role connection to inspect its target
backend. Concurrent client probes have individual cooperative deadlines and
error diagnostics. Native Windows selection/reset uses the runner's intended
WSL interface instead of accidentally hardcoding localhost forwarding.

An earlier combined native Windows run exceeded its 120-second process limit.
That runner did not retain partial client diagnostics, so its precise cause is
not established. The runner now preserves that output on timeout; the failed
run remains diagnostic evidence, separate from the subsequent frozen-source
passes. Interface controls passed on both routes with similar selection-test
durations, so they do not establish forwarding as the cause of this timeout.
No benchmark measurements were rerun, and these passes are not a claim of full
libpq parity or complete deployment soak qualification.

## Transport and TLS Clock Checkpoint, 7 October 2026

- Windows MSVC 19.44 and WSL2 GCC 15.2: affected PostgreSQL unit/protocol tests
  and independent SCRAM interoperability passed in Debug, Release and ASan.
- Real PostgreSQL 18.6 qualification passed in all six configurations. Windows
  executables ran natively against the disposable WSL server, not under Linux
  emulation. The server used an ordinary, non-superuser client role.
- Both runtime schedulers passed four-worker runs with 64 concurrent client
  probes per scheduler, alternating plaintext SCRAM and verified TLS/mTLS with
  required SCRAM channel binding. No runtime dependency was added to the library.
- Real-server cases cover SQL recovery, NULL/text/binary values, prepared metadata,
  duplex batches, COPY streaming/failure, streaming rows, portal fetches, server
  cancellation/reuse, blocking cross-thread cancellation and startup diagnostics.
- Independent pipelines passed in all six configurations, including the
  four-worker probes above. Cases cover correlated commands/barriers, prepared
  statements and portals, multiple Syncs, abort state across flushes, explicit
  transaction rollback, interleaved queueing/consumption and 4 MiB duplex traffic.
  Admission/retention bounds, deferred-task leases, terminal failures and the
  blocking facade have permanent regressions. Early malformed replies while a
  writer is pending preserve the original protocol error after draining the
  cancelled sibling, for both pipelines and batches.
- Raw COPY BOTH and explicit physical/database replication startup passed the
  same six configurations. Native duplex fixtures transfer 4 MiB each way with
  queued writers, independent half-closes, post-stream metadata/results and
  deferred-task leases. Malformed formats, data after receive completion, stream
  SQL errors and cancelled readers fail terminally.
- Real physical streaming covers IDENTIFY_SYSTEM, status/keepalive exchange,
  send completion, both terminal command results and connection reuse. Database
  replication startup permits ordinary SQL. Plaintext and verified mTLS/PLUS
  pass for async and blocking APIs, using a separate non-superuser role with
  replication privilege. Both four-worker schedulers also pass eight submitted
  replication jobs each, in addition to the 64 ordinary probes above.
- Physical password-file loading matches `replication`, not an ordinary
  database entry. Synthetic multi-host cases cover physical versus disabled/
  database modes; real connections use a file containing conflicting entries,
  deleted before connection establishment. Returned Options own the selected
  credential. The wrong-entry behavior was reproduced before the fix.
- Connection qualification now includes a disposable streaming standby: all six
  session targets, ordered failover, numeric address pinning and explicit reset
  pass for plaintext and verified TLS/mTLS. Four-worker probes select the primary
  after rejecting the standby, including cancellation against the selected peer.
- Password/TLS/certificate rejection stops selection rather than falling through.
  Attempt deadlines drain before failover; parent cancellation stops further
  attempts. Reset invalidates session resources and can recover a closed session;
  failed authentication preserves its diagnostic while leaving the session closed.
- Deferred query, prepare, COPY, row, portal, notification, finish and large-object
  Tasks block reset before they start. Frame-owned private leases also retain
  metadata across row callbacks and protect gaps between chunked operations.
  Busy/invalid reset does not destroy the current session.
- Owning URI/keyword parsing and typed conversion passed the same six
  configurations, including decoded credentials, multi-host/IPv6 lists,
  overridden settings, resource bounds and explicit unsupported/security modes.
  Real-server parsed configurations cover plaintext and verified mTLS/PLUS,
  LATIN1 startup, UTF8-only quoting rejection and explicit reset with raw TLS
  credential options. Parsing nonexistent TLS paths succeeds without opening
  them; connection establishment performs the credential loading.
- Explicit configuration loading passed all six configurations. Isolated
  processes cover environment/service/connection-string precedence, literal
  service values, user/system discovery, UTF8 Windows paths, escaped/wildcard
  password entries, per-host credentials, bounded regular files and Linux
  password permissions. Unsupported ambient security settings fail unless an
  explicit supported mode overrides them. Startup GUC identity/protocol fields
  and malformed per-host credentials are rejected before network setup.
- Real-server loaded options select the reachable host's password, apply startup
  settings and reset successfully over plaintext and verified mTLS/PLUS. The
  password file is deleted before connecting: the returned Options own the data
  and connect/reset do not silently reread ambient configuration. Other hosts'
  passwords are removed from the selected session configuration.
- Fast-path cases cover text/binary/NULL values, server SQL errors and reuse.
  Fake backends cover missing, truncated and duplicate FunctionCall responses;
  malformed/incomplete ordinary responses and oversized/cancelled exchanges
  also fail terminally.
- Quoting rejects malformed UTF-8 and NULs; real-server round trips cover quote,
  backslash and injection-shaped text with both string-conformance settings.
  Non-UTF8 client encoding is rejected by the quoting helpers.
- Large objects cover owning/span reads, chunked writes, 64-bit sparse positions,
  truncation position, explicit OIDs, Unicode client-file paths and file errors.
  Function discovery is tested with a shadowing temporary OID type/search path.
- TCP keepalive and Linux TCP user-timeout controls passed native IPv4/IPv6
  regressions in all six configurations, including native readback, invalid
  ranges, partial-update behavior, closed sockets and no asynchronous submissions.
  Windows rejects nonzero PostgreSQL user timeout before network setup rather
  than substituting a different native timeout. Parsed/service-loaded options
  and real plaintext/mTLS replication connections cover the transport settings.
- Physical and database replication sessions reject extended queries, prepared
  statements, portals, batches, pipelines and fast-path calls locally. Async and
  blocking regressions check that rejection leaves the connection usable for
  simple queries. Replication option spellings are ASCII case-insensitive.
- That frozen source snapshot passed all 20 build/test/packaging gate groups:
  Windows selected 13 non-packaging CTest entries and Linux selected 15 in each
  of Debug, Release and ASan. Native Windows real-server qualification also
  passed separately in all three configurations.
- Linux ASan passed five repetitions of each PostgreSQL test entry after the
  transport/restriction and TLS clock corrections: 20 passes in 47.00 seconds,
  including real-server runtime/file cases. Additional TLS soak results and the
  attributed clock failure are recorded in [TLS release gates](tls-release.md).
- TCP-only, TLS-only, PostgreSQL-only and all-component packaging passed on
  Windows and Linux: four selected tests per platform, isolated builds,
  relocated consumers and standalone public headers. The export graph still
  has no Runtime or libpq library dependency.

The Windows Release non-packaging suite also passed all 18 entries earlier in
this checkpoint. Final selections reflect the PostgreSQL/Runtime build roots,
not every optional module or comparison. No new benchmark measurements or TLS
security assessment are implied by these correctness passes.

## Pipeline Row Chunks: 2026-10-07

Pipeline execution now supports owning single-row and bounded row-chunk events.
Chunks keep their command ID and set `complete=false`; only the terminal event
releases command admission. Buffered execution remains the default. Blocking
streaming uses `start()` and `next()` on the caller's Context, without helper
threads or background progress.

The final implementation passed these nonempty, exact-count gates:

- Windows Debug, Release and Release-ASan: seven PostgreSQL CTest entries per
  configuration, plus native Windows disposable real-server qualification in
  all three configurations.
- Linux Debug, Release and Debug-ASan: ten PostgreSQL CTest entries per
  configuration, including real-server and filesystem/abstract Local coverage.
- PostgreSQL-only builds without Runtime: the controlled protocol suite passed
  once on each platform.
- ASan controlled protocol suites: five consecutive repetitions per platform.
- Relocated component packaging: nine CTest entries per platform, including
  isolated builds, public-header probes and relocated consumers.

Controlled peers cover owning metadata, command admission, oversized rows,
delivery-count and retained-byte backpressure, cancellation, consumer failure,
SQL errors after published rows, discarded incomplete chunks, aborted commands
and explicit Sync recovery. Four-worker fixtures cover both schedulers and both
Windows I/O layouts; Linux uses its supported sharded layout. Real PostgreSQL
fixtures cover prepared binary/NULL rows, mixed buffered commands, zero rows,
session reuse and blocking-driver move/destruction. These are correctness tests,
not throughput or latency measurements.

A private implementation probe exposed a logical budget leak when an unstarted
publication Task was destroyed: 232 retained bytes remained charged even though
the data allocation was reclaimed. Delivery objects now own their charges before
initial suspension and release them through moves/destruction. The identical
probe reports zero retained bytes, and the final gates above ran after that fix.
Queued deliveries are drained before their budget and wake channels die.

Exploratory fixture errors are retained and attributed: a libpq comparator
incorrectly expected the command tag on its empty terminal result rather than
the final partial chunk; scratch compilation required corrected declarations,
assertion grouping and private dependency imports. An initial WSL invocation
misinterpreted a test-filter pipe, so its Linux tests did not execute. A later
packaging selector matched zero tests and is not packaging evidence. The final
runner uses `wsl --exec`, `--no-tests=error` and exact selected-count validation.

Ignored evidence is stored under `benchmarks/results/`:
`postgres-pipeline-streaming-final-20261007.json` contains the 23 final gate
steps and the reproduced accounting failure/fix. Earlier probe and qualification
JSON files retain failed fixture/runner attempts. The matching
`postgres-pipeline-streaming-source-20261007.zip` and `.json` preserve source
and fixture-binary hashes; `postgres-pipeline-streaming-exploration-20261007.zip`
preserves the temporary probes before their removal.

No performance benchmarks were run for this checkpoint. Transport-only pipeline
send/receive separation is still pending: `flush()` and blocking `start()` drive
full-duplex exchanges, not a transport-only acknowledgement. This checkpoint does
not establish complete libpq parity or an independent security assessment.

## Split Pipeline Transport: 2026-10-07

This follow-up adds `Pipeline::send()` and `receive()` while preserving duplex
`flush()`. Sending completes without reading results or adding implicit Sync;
receiving publishes correlated owning results without writing. Either direction
can reserve the next bounded batch, so receiver-first scheduling is supported.
Later batches can be sent while earlier responses are being read, and an earlier
Sync cannot clear later submitted synchronization debt.

Blocking pipelines support one bounded `send()`/`receive()` window and
`start_receive()`/`next()` for incremental results, on the existing caller-thread
Context. Unsent blocking reads and mixed duplex/split operations are rejected.
Bulk work should use duplex progress, not a blocking write ahead of its reader.
On Runtime, split transport directions stay in one serialized `when_all`
producer; separate stealable roots must not share Connection protocol state.

All 23 final-source gate steps passed, with nonempty exact-count CTest validation:

- Windows Debug, Release and Release-ASan: seven PostgreSQL CTest entries each,
  plus disposable real PostgreSQL qualification for each native configuration.
- Linux Debug, Release and Debug-ASan: ten PostgreSQL CTest entries each,
  including real PostgreSQL and filesystem/abstract Local sessions.
- PostgreSQL-only, without Runtime: the controlled suite passed on each platform.
- ASan controlled suites: five consecutive repetitions on each platform.
- Relocated components: nine packaging entries on each platform, including
  isolated PostgreSQL imports and public send/receive/start_receive link probes.

Controlled peers withhold responses until two sends have completed, proving the
send acknowledgement is independent of query results. They cover receiver-first
reservation, competing readers, later Sync debt, multi-megabyte duplex traffic,
early malformed-response cancellation of a blocked writer, original-error
preservation, deferred leases and streaming backpressure/recovery. Four-worker
fixtures cover both schedulers and both Windows I/O layouts; Linux uses sharded
I/O. Real-server fixtures cover multiple sends, prepared binary/NULL chunks,
blocking buffered/incremental receives, driver moves and session reuse over
plaintext and verified mTLS. Existing duplex fixtures remain in the gates.

The exploratory native Windows direct-address connection was refused because
the persistent scratch server listens on loopback only. A missing failed-connect
guard then dereferenced an error-valued expected in the scratch fixture. The
guarded probe reports the refusal explicitly; Windows loopback and Linux
loopback blocking probes passed without changing that server. Final Windows
qualification uses disposable clusters with explicit Windows access, not that
persistent fixture. Both failed scratch attempts remain in ignored evidence.

`benchmarks/results/postgres-transport-qualification-20261007.json` records the
final gates and production-source hashes. The matching
`postgres-transport-source-20261007.zip` and `.json` archive source and fixture
binary hashes; `postgres-transport-exploration-20261007.zip` preserves deleted
scratch sources/scripts. Probe JSON files retain the initial setup failures and
their attributed controls. No performance benchmarks were run.

Transport send/receive separation is now implemented. Native readiness handles,
partial-write polling statuses, complete libpq parity and an independent security
assessment are not claimed by this checkpoint.

## Independent Fixtures

The bounded SCRAM fixture computes client/server proofs independently using
Python's standard cryptographic interfaces. It checks channel-binding bytes,
Unicode SASLprep, bad proofs/nonces, duplicate fields and excessive iteration
work. It is not libpq posing as an independent protocol oracle.

Before adding permanent parser regressions, a deleted scratch probe compared
supported grammar cases with `PQconninfoParse`, exercised live plaintext and
verified TLS/PLUS configuration, and ran 100,000 deterministic byte-string
mutations. That is exploratory coverage, not an exhaustive fuzz campaign or
a libpq dependency in the library/test suite. The permanent tests are standalone.

Configuration-loader scratch probes passed on Windows and Linux before their
permanent regressions were added, including actual plaintext/TLS password-file
connections and environment-selected session settings. Those probes and their
temporary build targets are deleted. The permanent file/environment suite uses
private temporary directories and child processes, so user PG variables and
credential files cannot influence its synthetic fixtures.

Deleted transport/restriction probes passed on native Windows and Linux before
permanent regressions were added. An initial option test incorrectly treated an
unquoted empty keyword value as empty; libpq-style whitespace after `=` instead
belongs to the following value. The fixture now uses explicitly quoted empties.
A doctest expression needed grouping to avoid its assertion decomposition.
Both failed attempts are retained alongside the final evidence, not retried as
performance samples. The supplemental TLS failure was independently attributed
and fixed before the final six-configuration run.

Deleted pipeline scratch probes independently compared ten correlated outcomes
with libpq pipeline statuses, on Linux and native Windows over plaintext and
verified TLS/PLUS. They also exercised duplex traffic, failure/recovery and both
four-worker schedulers before permanent regressions were added. This is protocol
qualification, not a new performance measurement or a libpq test dependency.

Native Windows large-duplex probes stalled through WSL localhost forwarding:
two process timeouts and three cooperative flush timeouts are retained in ignored
diagnostic evidence. Native Windows loopback fixtures and Linux real-server
probes passed; native Windows real-server probes passed through the direct WSL
interface. Those controls isolate the failing path, not its exact forwarding
component or mechanism. The real-server runner now discovers that interface,
binds the disposable server to it and loopback, and authorizes only the Windows
gateway address and test role. Verified TLS still checks the intended hostname.
It does not listen on all interfaces or certify a native Windows database server.

GCC 15.2 hit an internal compiler error on a nested initializer-list expression
inside a new coroutine regression. Naming the command collection before the
await avoids that compiler defect without changing library behavior. An initial
packaging selector matched zero tests; that run was invalidated, the selector
was corrected, and both actual packaging tests passed on each platform.

The opt-in real-server runner creates private temporary primary/standby clusters, random
credentials, a verified mTLS profile and an ordinary login role. It stops the
server and removes the cluster even when a partially successful start leaves a
postmaster PID file. Both clusters are stopped even when one cleanup fails.
Inherited libpq environment settings are removed from setup commands so an
external PGHOSTADDR or service file cannot redirect the fixture. Client-file
fixtures are removed after use. No external
database, public DNS service or production credential is required.

During fixture hardening, PostgreSQL rejected an attempt to demote its bootstrap
superuser. That failure was in the setup script, not attributed to Weave's
transport. The runner now initializes a separate administrator and creates the
ordinary test login, then the full affected real-server gates pass. No unexplained
setup failure is being carried forward.

## Notice Callback Checkpoint, 7 October 2026

`NoticeHandler` owns a move-only `noexcept` callable. It can be installed before
startup, replaced synchronously with the previous callable returned for explicit
save/restore, and retained across Connection moves and successful/failed reset.
Only validated server notices invoke it; default bounded queuing remains intact.
[API, execution and lifetime contract](postgres-notices.md).

Exploration also found that an unstarted reset Task borrowed its Connection
without acquiring a lease. Reset now acquires that lease at Task creation and
releases it before replacing the old implementation. Competing reset and handler
replacement are rejected while a deferred reset exists; dropping it performs no
I/O and preserves the session.

All 23 final-source qualification steps passed with exact nonempty CTest counts:

| Gate | Windows | Linux / WSL |
| --- | --- | --- |
| PostgreSQL Debug / Release / ASan entries | 8 / 8 / 8 | 11 / 11 / 11 |
| Controlled notice cases per configuration | 8 | 6 |
| PostgreSQL-only unit/notice entries without Runtime | 2 | 2 |
| ASan unit/notice repetitions | 5 each | 5 each |
| Relocated components and public callback link probes | 9 | 9 |
| Native Windows disposable real-server Debug/Release/ASan gates | 3 | Included in suites |

The independent peer covers startup and pre-error notices, fatal startup during
reset, receiver save/restore and exact capture destruction, deferred query/reset
and pipeline leases, preserved earlier queues, default overflow after clearing,
and malformed/oversized messages rejected before invocation. Reentrant receiver
replacement, close and cancellation must return `busy`. Runtime uses 16 independent
sessions on four workers per scheduler/layout: Windows sharded/shared, Linux sharded.
Native libpq oracle probes matched notice ordering and receiver retention through
reset on each platform; libpq is not a library or permanent-test dependency.

Real PostgreSQL probes raise NOTICE/WARNING and verify SQLSTATE, callback delivery,
reset retention and restoration of default queuing. They run over plaintext,
verified mTLS, Linux Local and 64 four-worker Runtime clients per scheduler.
The main real-server Runtime fixture uses the default sharded layout.

An initial native Windows ASan run timed out for client 63 in the 64-client affine
fixture's 20-second whole-workload deadline. It emitted no sanitizer report or
server connection-limit error; the original log did not identify the operation
where that client was waiting. Per-stage failure diagnostics were added, without
changing the workload, concurrency or deadlines. The instrumented diagnostic
and final native ASan runs passed, but they do not establish the cause of the
earlier timeout. That failure and its source snapshot remain separate retained
evidence, not a passed run or a claimed library fix.

Raw evidence is retained under ignored `benchmarks/results/postgres-notice-*`:
initial/final gate records, stage diagnostic, independent/libpq probes, a complete
source archive with fixture executable hashes, and archived exploratory sources.
Ad hoc source/targets are removed after archiving. No performance measurements
were rerun for this API/correctness change. Encoding, authentication, event/tracing
and full release qualification gaps in the parity matrix remain open.

## Encoding and Quoting Qualification (2026-10-07)

The UTF8-only restriction described in earlier historical gates is superseded by
[typed encoding control and encoding-aware utilities](postgres-encoding.md).
All 26 final-source qualification steps passed with exact, nonempty CTest counts:

| Gate | Windows | Linux / WSL |
| --- | --- | --- |
| PostgreSQL Debug / Release / ASan entries | 9 / 9 / 9 | 13 / 13 / 13 |
| Controlled encoding cases per configuration | 12 | 10 |
| PostgreSQL-only unit/notice/encoding entries without Runtime | 3 | 3 |
| ASan unit/notice/encoding repetitions | 5 each | 5 each |
| Relocated components and public encoding link/header probes | 9 | 9 |
| Native Windows disposable real-server configurations | Debug, Release, ASan | Included in suites |

Independent protocol peers verify invalid enum rejection without writes, observed
SQL ParameterStatus changes, missing/unrecognized metadata, contradictory setter
results, SQL-error recovery, cancellation, pipeline admission, reset and the borrow
retained by unstarted setter Tasks. Four-worker controlled modes use 32 independent
sessions per scheduler/layout: Windows sharded/shared and Linux sharded. The
blocking facade is checked separately. These are correctness cases, not benchmarks.

Real PostgreSQL 18.6 probes select all 42 encodings and require actual server
responses. UTF8 databases reject unavailable MULE_INTERNAL conversion explicitly;
SQL_ASCII databases reject non-ASCII client-only input with SQLSTATE 22021.
Valid multibyte/single-byte samples round-trip exact values and quoted identifier
names, including Shift JIS/BIG5/GBK continuation backslashes, GB18030 four-byte
characters and EUC prefixes. Each runs with standard_conforming_strings on/off,
over plaintext and verified mTLS, plus Linux Local. Blocking queries and 16
independent four-worker sessions per available scheduler/layout are included.

Native libpq correctness oracles on Windows 18.4 and Linux 18.6 cover 75,793
inputs per encoding: empty, every one-/two-byte combination and 10,000 seeded
longer samples. Each platform checks 3,183,306 inputs and 6,366,612 literal/identifier
comparisons, normalizing libpq's optional E prefix. The only accepted quote
differences are Weave's documented stricter NUL and continuation-quote rejection.
Valid first-character byte sizes and legacy widths match the libpq oracle;
UTF8 widths use ICU data and are not asserted against a different Unicode table.
This is bounded corpus evidence, not exhaustive fuzzing or an independent audit.

An initial exploratory oracle exposed MULE_INTERNAL's special width for ASCII
controls; the scanner now handles that encoding before the common control rule.
A real-server probe also initially expected SQL_ASCII/client-only high-byte input
to round-trip; PostgreSQL's rejection is now an explicit expected SQL error, not
a hidden fallback or guessed quoting workaround. Both failed probes are retained.

The first encoding fixture setup timed out during a default paced backup
checkpoint after creating the extra database. Disposable test backups now request
`--checkpoint=fast` rather than waiting for deliberate checkpoint pacing. This is
test setup only; operation deadlines, concurrency and library timeouts were not
relaxed. The earlier unrelated notice-phase ASan timeout remains separate evidence
with no proven cause.

Ignored `benchmarks/results/postgres-encoding-*` holds the gate records, independent
oracles, failed explorations, current source/fixture hashes and archived scratch
sources. Temporary probe sources and targets are removed after archiving. No
performance measurements were rerun for this feature.

## Credential Lifetime Qualification

Exploratory allocation witnesses confirmed that destroying an unstarted connect
Task released its global password, per-host password and TLS private-key
passphrase without cleansing them. A guard inside a lazy coroutine body cannot
run before that body starts. Connect and reset now acquire a private owning
cleanup parameter before initial suspension, including across internal host,
transport and reset handoffs. The wrapper has the same size as Options and
introduces no shared allocation or additional coroutine. The public API is unchanged.

Blocking setup also guards its Options before Context creation. TLS factories
now protect their passphrases before early validation and provider setup. Internal
moves clear retained inline source characters using bounded string operations,
not writes beyond a string's logical size.

Sixteen exploratory cases passed on each platform. Permanent isolated executables
check owned allocations inside replacement delete, before deallocation; they do
not read freed memory or inject allocation hooks into library targets. Coverage
includes both connect/reset overloads, dropped Tasks, rejected submissions,
validation failure, cancellation before execution, blocking validation and TLS
validation/policy/identity rejection. Known inline character bytes are checked
only inside still-live MSVC/GCC string objects, without inspecting padding.

Frozen C++/CMake source passed all 34 supplemental gates:

| Gate | Windows | Linux / WSL |
| --- | --- | --- |
| PostgreSQL Debug / Release / ASan entries | 10 / 10 / 10 | 14 / 14 / 14 |
| TLS Debug / Release / ASan entries | 3 / 3 / 3 | 4 / 4 / 4 |
| PostgreSQL-only unit/notice/encoding/credential entries | 4 | 4 |
| ASan PostgreSQL repetitions | 5 per selected entry | 5 per selected entry |
| ASan TLS repetitions | 10 per entry | 20 per entry |
| Relocated components and public-header checks | 9 | 9 |
| Native Windows disposable main/encoding server gates | Debug, Release, ASan | Included in suites |

Real-server gates retain concurrent four-worker coverage of both schedulers and
all supported I/O layouts. Existing protocol, cancellation and authentication
restrictions are unchanged. Initial failing allocation probes are retained in
ignored `benchmarks/results/*credentials*-20261007.json` alongside final raw gates
and source/fixture hashes. A Linux exploratory launch initially passed a Windows
backslash path to WSL; that runner error is retained separately and corrected to
a POSIX path, with no library or workload change. Scratch sources are archived
and removed after qualification. No timing benchmarks were rerun.

Cleansing remains best-effort. Public Options and TLS options are ordinary values;
caller copies, compiler/library temporaries, allocator history, protocol/crypto
intermediates, crash dumps and swapping are not completely erased by these changes.
These gates establish the specified ownership paths, not a security audit or full
libpq feature/release parity.

## Authentication Buffers and TLS Duplex Requalification

Allocation witnesses identified three additional authentication cleanup paths:
password-message growth released plaintext before the final packet cleanup;
MD5 concatenation released plaintext and digest text through ordinary strings;
and SASLprep copied its normalized password on return. The Windows and Linux
negative probes are retained, not replaced by passing runs.

Private authentication text, wire and ICU storage now clears its complete
allocation before deallocation, including vector growth and discarded capacity.
Move-only arrays clear fixed digest, key, signature and proof storage on failure,
move and destruction. Ordinary query/result storage and the public API are
unchanged. Independent password-response and Unicode SCRAM fixtures still check
the protocol, rather than relying only on Weave's own calculations.

The first Windows ASan real-server gate failed at client 17's pipeline stage:
startup finished in 332 ms, notices/reset in 335 ms, and the pipeline remained
unfinished at its original 20-second whole-client timeout. No sanitizer error
was reported. Two instrumented full runs and eight concurrent-only runs did not
reproduce that timeout; they are diagnostic controls, not proof of its cause.

Review then found an independently reproducible TLS duplex deadlock: a reader
with no outgoing records waited for a transport send held by the writer. A
controlled peer sent readable application data while that send was blocked;
both TLS versions timed out with the original adapter. The corrected adapter
lets these reads progress without weakening write/shutdown completion barriers.
The probe passes both TLS versions and 20 process repetitions per platform,
and the confirmed behavior is covered by a permanent regression.

Fresh frozen source passed all 34 authentication/TLS supplemental gates on
2026-10-08, with the same entry counts as the credential-lifetime table above.
That includes all six Debug/Release/ASan configurations, native Windows
disposable server gates, no-Runtime builds, five PostgreSQL ASan repetitions,
10 Windows/20 Linux TLS ASan repetitions, and nine packaging entries per
platform. Concurrency, payloads and the original real-server deadlines were not
relaxed. This establishes the current gates, not a retrospective attribution
of the earlier timeout or complete PostgreSQL release readiness.

Raw failures, diagnostics, final gates and source/probe archives are retained
under ignored `benchmarks/results/*auth*-20261008*` and
`benchmarks/results/*duplex*-20261008*`. Exploratory sources are removed after
archiving. No timing benchmarks were rerun. Owned-buffer cleansing remains
best-effort; caller copies, all compiler/provider temporaries, swapping and crash
dumps are not covered.

## SCRAM Key Passthrough

Typed `ScramKey` values own 32 decoded credential bytes. Keyword/URI parsing and
explicit service loading accept the two middleware key options. Both keys allow
passwordless SCRAM/PLUS; partial keys derive the missing key from the selected
password. Client-key-only input without a password is rejected before the initial
response. Proof verification, channel binding, authentication restrictions and
challenge bounds remain mandatory.

Windows and Linux ASan exploratory fixtures passed:

- 29 independent protocol cases, including both keys, partial keys, irrelevant
  passwords, parsing, changed challenge salt, incorrect keys/proofs, malformed
  challenges, method rejection and incomplete authentication.
- 32,832 exact parser-oracle inputs per platform, generated from 4,096 independent
  32-byte values, malformed variants and every other decoded size from 0 to 64.
  Copy/move/destruction checks accompany the corpus.
- Credential-allocation witnesses across dropped connect/reset Tasks, rejected
  Context/Runtime submissions, pre-start cancellation and setup failures.
- Eight real-server blocking profiles and 64 simultaneous sessions on each
  four-worker scheduler, mixing plaintext, verified mTLS/SCRAM-PLUS, both keys,
  partial keys and password controls. Existing pipeline, COPY, cancellation,
  failover and reset workloads and their deadlines were retained.

The real-server runner derives keys independently from its owned disposable
role verifier and verifies them against PostgreSQL's stored/server keys. It never
reads or changes an existing deployment's role credentials. Permanent coverage
adds owning-value/parser/service tests, allocation witnesses and the 16 new
protocol modes to existing executables. Opt-in real-server qualification adds
the blocking/Context profiles and mixes the same credentials into its existing
64-client scheduler workloads.

All 32 initial frozen-source gates passed on 2026-10-08: Windows and Linux
Debug/Release/ASan PostgreSQL and TLS suites, native Windows disposable main and
encoding runners, four PostgreSQL-only entries per platform, five ASan
repetitions of five selected entries, and nine packaging entries per platform.
The final startup change discards session key copies at ReadyForQuery. Fresh
frozen source then passed 21 PostgreSQL gates across all six configurations,
including the real-server, no-Runtime and repeated-ASan coverage. Public API,
TLS engine and packaging sources were unchanged between these checkpoints.

Nine independent libpq controls passed per platform with runtime versions
18.4 on Windows and 18.6 on Linux. A confirmed compatibility difference remains:
with only the client key supplied, those versions reject a valid server proof
even when a password is provided. Weave derives the missing server key from the
password and verifies that proof; it does not skip verification.
See [the key API and scope](postgres-connections.md#scram-key-passthrough).

Harness failures are retained separately: a parsed host port initially remained
5432; initial Windows controls loaded an ambient libpq 16.4 DLL; a CMake DLL copy
used a directory list instead of its singular path; and text-mode Windows stdin
sent CRLF to the Linux corpus. The corrected runs use an explicit fixture port,
pinned libpq DLL and binary corpus input. None required relaxing library checks
or excluding failed samples.

Raw results, failed attempts, source/probe hashes and archives are retained under
ignored `benchmarks/results/postgres-scram-keys-*-20261008*`. Exploratory sources
are removed after archiving. No timing benchmarks were rerun. These are bounded
correctness/security gates, not a security audit or full libpq release parity.

## Native GSSAPI/SSPI Provider Checkpoint

This is the historical provider-only checkpoint. Startup integration is recorded
separately below; its later evidence supersedes the integration limitations here.

The PostgreSQL module now contains private native provider engines: MIT GSSAPI
on Linux and system SSPI on Windows. Public PostgreSQL GSS/SSPI requirements
remain unsupported and fail before connection setup. Native provider success
is not PostgreSQL startup or encrypted-transport success.

The providers own credentials, names, contexts and cleansing token buffers.
They bound tokens to 64 KiB and negotiation to 64 steps, reject invalid
host/service components, and become terminal after a failed continuation.
Delegation is opt-in. Linux explicitly selects Kerberos and supports an explicit
credential cache. Windows distinguishes Kerberos from NTLM after Negotiate;
an NTLM mutual-auth flag is not treated as proof of the server's identity.
Unknown completed SSPI mechanisms fail closed.

On 2026-10-08, the frozen exploratory provider checkpoint passed:

- 220 Linux native-provider cases under ASan, using an owned temporary KDC,
  principals, keytab and cache. Cases cover mutual Kerberos success, malformed
  and truncated tokens, tampered encrypted server proof, missing explicit cache,
  wrong service, empty/oversized input, thread handoff and opt-in delegation.
- 140 native Windows SSPI cases under ASan: local NTLM success, invalid tokens,
  strict mutual-auth rejection, missing Kerberos credentials, input bounds and
  thread handoff. Positive Windows Kerberos/domain authentication is not qualified.
- A separate bounded offload prototype with two native workers and 64 queued
  calls: over 130,000 accepted completions across five repetitions per platform,
  Context execution, 32 concurrent roots on four-worker runtimes, both schedulers,
  and Windows sharded/shared IOCP. Task cancellation, Context shutdown and queue
  saturation passed. The publication guard retains frame/Context/executor lifetime
  until the foreign publisher leaves the continuation-routing call.

The prototype is not a production authentication executor. Windows impersonation,
credential snapshots, native cleanup and provider calls must be integrated and
qualified together before use in startup. Native calls can wait on a KDC;
cancellation must drain them rather than destroy active coroutine frames or promise
a hard deadline. No PostgreSQL GSS authentication, GSS encryption or complete
libpq authentication parity is claimed at this checkpoint.

The initial frozen-source gate passed all 24 steps, including 101 selected CTest
entries: six PostgreSQL configurations, nine packaging entries per platform,
and a GSSAPI-disabled Linux build plus relocated consumer. After permanent
Kerberos cases and the final Windows mechanism restrictions, all 24 retirement
steps passed: six configurations, no-Runtime builds, disabled-GSSAPI coverage
and ten repeated native Kerberos ASan entries. Permanent Linux cases also check
that delegation produces an owned acceptor credential only when requested.
The final fixture-only timeout-redaction and intentional-client-failure cleanup
checks passed, followed by fresh native fixture runs in all three Linux builds.
Compiled sources were unchanged. The first redaction probe's fixed secret literal
appeared in its own traceback source line; the corrected probe uses a runtime
generated value and retains that failed attempt separately.

The Linux diagnostic investigation confirmed that a mapped minor status can
describe native zero even when the major status rejects a token. Error codes
now preserve the major failure; bounded major/minor diagnostics are captured
on the provider thread instead of depending on its later thread-local state.
Initial fixture failures and misleading-diagnostic attempts remain in the raw
record. The owned KDC fixture explicitly disables short-host search-suffix
qualification; it never changes global realm or credential-cache configuration.

Raw results and source/probe archives are retained under ignored
`benchmarks/results/postgres-gss-*-20261008*`. Exploratory source files are removed
after archiving. These are correctness/lifetime checks, not benchmarks or a
security audit.

## Run Locally

Configure `WEAVE_MODULES=postgres;runtime`, `WEAVE_BUILD_TESTS=ON`, and optionally
`WEAVE_POSTGRES_SERVER_TESTS=ON` with `WEAVE_POSTGRES_SERVER_BIN` pointing at
`initdb`, `pg_ctl`, `psql` and `pg_basebackup`. Correctness CI runs the bounded protocol/SCRAM
fixtures without needing a database server. Benchmarks remain off.

Linux's private provider requires `krb5-gssapi` development files by default;
`WEAVE_POSTGRES_GSSAPI=OFF` removes that dependency. The permanent native Kerberos
fixture is separately opt-in with `WEAVE_POSTGRES_KERBEROS_TESTS=ON` and requires
`krb5kdc`, `kadmin.local`, `kdb5_util` and `kinit`. It runs unprivileged against
its own temporary realm, with no production credentials or global configuration
changes. Select `^weave_postgres_kerberos$` to run just that fixture.

```text
ctest --test-dir <build> -R weave_postgres --output-on-failure
ctest --test-dir <asan build> -R weave_postgres --repeat until-fail:5 --output-on-failure
```

For native Windows qualification with a Linux PostgreSQL server, run the opt-in
runner from WSL with `--windows-client`, a `/mnt/c/.../weave_postgres_live.exe`
executable and the Linux `--server-bin`. ASan's compiler runtime DLL must be
available to the native executable. This is supplemental local coverage, not a
hosted Windows CI benchmark or native Windows server certification.
Add `--scram-keys` for the main runner's passthrough profiles; configured opt-in
CTest main-server entries enable it automatically. Other runner variants do not
consume this extra credential input.

For the mixed backup/logical runner, select `weave_postgres_exchange_live.exe`
and add `--exchanges`. This configures logical WAL and test roles only inside
the runner's owned disposable clusters.

For encoding qualification, select `weave_postgres_encoding_live.exe` (or the
Linux executable) and add `--encoding`. The extra SQL_ASCII database is created
only in the runner's private cluster. Normal Windows correctness CI needs no
database server and runs the bounded encoding unit/protocol cases.

## Native GSS Startup Integration

Qualified on 2026-10-08 with an explicitly created, bounded `pg::GssContext`.
Native acquisition/negotiation/destruction run on provider workers, while
continuations return through the captured Context/executor. Admission reserves
cleanup capacity per session. Cancellation drains native work; shielded cleanup
also survives Context shutdown. Ordinary startup retains its previous coroutine
graph unless a provider context was explicitly supplied.

Linux's owned realm and PostgreSQL 18.6 cluster first pass a libpq/psql GSS
control with TLS and GSS encryption disabled. Twenty-two client profiles cover
Context and blocking queries, prepared execution, batching/reset, captured
default-cache selection, delegation, missing credentials/wrong services,
authentication/channel-binding policy, forged AuthenticationOk, duplicate or
mixed challenges, empty/corrupt/oversized continuations, pre-cancellation,
in-flight cancellation, Context shutdown, saturation and 32 concurrent clients
on both four-worker schedulers. A test-only interposer delays native initiation
and destruction and signals actual native entry before cancellation; no
production delay hook or provider injection API is installed.

Windows's independent native SSPI acceptor speaks PostgreSQL startup over
verified TLS. It checks authentication-policy handling, native mechanism
selection, strict rejection of non-mutual NTLM, identification-token rejection,
and use of the duplicated effective token after caller reversion/original-token
closure. Runtime-enabled entries run 104 startup cases: both mutual policies
on Context plus 32 clients on each scheduler. No-Runtime entries run 40 cases.
The acceptor inspects the actual mechanism instead of assuming every deployment
selects NTLM. Positive Windows domain/Kerberos authentication is still unqualified
on this non-domain machine; local SSPI is not a substitute for that evidence.

| Selected CTest Coverage | Windows | Linux |
| --- | --- | --- |
| PostgreSQL Debug / Release / ASan | 11 each | 16 each |
| TLS Debug / Release / ASan | 3 each | 4 each |
| Component isolation, relocation and standalone headers | 9 | 9 |
| Selected PostgreSQL-only/no-Runtime entries | 5 | 6 |
| GSSAPI-disabled PostgreSQL and relocated consumer | Not applicable | 10 + 1 |
| Repeated native startup ASan entry | SSPI, 10 repetitions | Real KDC/PostgreSQL, 10 repetitions |

The six-configuration baseline passed all 26 steps and 120 selected CTest
entries. The final alternate-build/repetition run passed all 13 steps, including
22 ordinary entries and 20 repeated executions. Both records hash 240 relevant
first-party source/build/test files. Later fixture-only changes isolate inherited
PG variables and publish success only after owned-process cleanup; they passed
the no-Runtime and repeated Linux ASan gates. The final Windows ASan test was
also rebuilt after removing an unnecessary private-header dependency.

Five additional fixture checks confirm setup/timeout output redaction, stopped
owned PostgreSQL, drained KDC children and removed private directories after an
intentional client-launch failure. No existing PostgreSQL cluster, global realm,
production credential or user configuration was changed.

Failed exploratory evidence is retained. A Linux link error was traced to
`com_err.h` lacking C++ linkage guards and fixed with a C-linkage include before
`krb5.h`. Fixture assertions were corrected for PostgreSQL Terminate/close_notify,
queued work cancelled before native entry and Context::run's cancellation result.
An initial packaging selector matched no tests and failed with `--no-tests=error`;
the corrected selector selected and passed all nine entries. These are attributed
failures, not omitted samples or evidence of a performance improvement.

Raw records and source/probe archives are ignored under
`benchmarks/results/postgres-gss-startup-*20261008.*`. Exploratory source files
were archived and deleted; permanent tests live under `modules/postgres/tests`.
No performance benchmarks were run for this security/correctness work.

Public [identity, cancellation and build semantics](postgres-gss.md) deliberately
do not promise hard native-call deadlines, immutable cache contents,
GSS-encrypted transport, full libpq parity or an independent security audit.

## Native GSS Record Protection

Qualified on 2026-10-08 as a **private transport foundation**, not an enabled
PostgreSQL GSS-encrypted connection. Native MIT GSSAPI and Windows SSPI now
provide bounded wrap/unwrap operations. Protected contexts require Kerberos,
mutual authentication, confidentiality, integrity, replay detection and sequence
detection. Linux computes the plaintext limit with `gss_wrap_size_limit`;
Windows uses the native security trailer/padding bounds. No cryptographic
algorithm is implemented by Weave.

An independent Linux MIT acceptor exercises nine protected-record profiles,
each with eight fresh exchanges: bidirectional empty/small/maximum-sized data,
outgoing bounds, tampering, truncation, empty/oversized incoming records,
integrity-only input, replay and sequence gaps. Failed incoming protection is
terminal. Four lifetime profiles cover 32 sessions each: Context success,
failed-task frame cleanup, four-worker affinity and four-worker stealing.
Successful sessions submit wrap and unwrap together through `when_all`, exercising
the per-session native gate and owning, synchronized metadata observations.
No-Runtime builds run the nine record profiles and both Context lifetime profiles.

A test-only native interposer tracks initiator contexts, marks I/O threads and
delays native deletion. It verifies matching acquisition/destruction counts,
that initiation/wrap/unwrap/deletion never execute on marked I/O threads, and
that final pool ownership can drain retirement after Context/Runtime destruction.
Idle retirement transfers a preallocated owning node, never a coroutine-frame
borrow or an owning pool reference on a provider worker. Explicit cleanup remains
shielded from task cancellation and Context stop. All session borrowers must
drain before cleanup/destruction; retirement does not permit active-frame deletion.

| Selected Coverage | Windows | Linux |
| --- | --- | --- |
| PostgreSQL Debug / Release / ASan | 11 each | 17 each |
| TLS Debug / Release / ASan | 3 each | 4 each |
| Initial component isolation, relocation and standalone headers | 9 | 9 |
| Selected no-Runtime PostgreSQL entries | 5 | 7 |
| GSSAPI-disabled PostgreSQL and relocated consumer | Not applicable | 10 + 1 |
| Final repeated native ASan entries | SSPI, 10 executions | Startup + protection, 20 executions |

The baseline passed 34 steps and 123 selected CTest entries. After the additional
duplex/failure probe and synchronized owning metadata refinement, all six
configurations were rebuilt and passed 30 steps and 105 PostgreSQL/TLS entries.
The alternate-build and repetition gate passed 15 steps: 23 ordinary entries
and 30 repeated ASan executions. Each gate hashes 226 module/build/test sources;
the final and retirement hashes match the archived source. The baseline's earlier
metadata implementation is explicitly superseded by the final gate.

Exploratory configure/include/build failures were attributed and corrected:
PowerShell split an unquoted CMake path argument, a probe used the wrong Context
header, and the audit target lacked its explicit C++23 requirement. The native
record and duplex/failure probes both passed. Raw evidence and source/probe
archives remain ignored under `benchmarks/results/postgres-gss-protection-*20261008.*`.
Exploratory source is archived and removed rather than left as extra tests.

Positive Windows domain/Kerberos protected-record interoperability remains
unqualified on this non-domain machine. At this foundation checkpoint, public
GSSENCRequest negotiation and transport integration were still pending. The
following integration supersedes that limitation. Existing startup authentication
gates also passed after the foundation change. No performance benchmarks were run.

## Native GSS Encrypted Transport

Implemented on 2026-10-08: typed `GssEncryption::disable|prefer|require`, explicit
provider admission, PostgreSQL GSSENCRequest negotiation, protected framing and
same-engine Context/Runtime/blocking operation. Successful GSS encryption takes
priority over TLS. `prefer` falls back only after an explicit server `N`; it never
retries after proof, record, socket or framing failure. Cancellation snapshots
retain endpoint/principal/provider ownership and always require encryption.

The owned Linux realm/PostgreSQL fixture now has independent libpq authentication
and encryption controls. Its encrypted suite verifies reported `pg_stat_gssapi`
encryption, 100 KB SQL, 200 KB results, a 300 KB parameter, duplex batches,
approximately 200 KB COPY, SQLSTATE 57014 cancellation, repeated requests, deferred cancellation
across reset, and a request that outlives both Connection and its original Context.
Both four-worker schedulers run 32 encrypted sessions. Single-slot reset also
works after synchronous close, proving deterministic native admission recycling.

An independent native acceptor exercises explicit decline, invalid/unauthenticated
negotiation replies, zero/oversized/corrupt native tokens, zero/oversized records,
partial headers/bodies, tampering, replay, sequence gaps and integrity-only input.
Native initiation, wrap and unwrap cancellation/Context-stop probes delay actual
provider entry, not merely a queued operation; cancellation drains before reset
or destruction. Saturation fails explicitly rather than silently oversubscribing.
The fixture runs 43 profiles with Runtime or 37 without it, including prior
authentication-only coverage. These are correctness tests, not benchmarks.

Exploratory failures were attributed to the probes: redundant COPY OUT completion,
confusing token-body length with startup length including its header, and expecting
a successful Context return after `request_stop()`. The successful probes retained
the original failure evidence. A gate selection also initially excluded the
benchmark-tooling unit test; that Python test validates the harness and performs
no measurements, so it remains in the correctness selection.

A two-host probe exposed an actual retry-policy gap: the existing generic timeout
rule advanced to the second host after sending a GSS proof. The new regression
test first reproduced that second attempt, then passed after making post-connect
timeouts terminal whenever GSS encryption is enabled. Ordinary connection timeout
failover and pre-connect/target-session selection remain unchanged. Explicit `N`
fallback to verified TLS also passed independently before entering the suite.

The final sequential gate passed 34 steps and 123 selected CTest executions:
PostgreSQL and TLS under Windows/Linux Debug, Release and ASan, followed by nine
component-isolation/relocation/header entries on each platform. The alternate
gate passed 15 steps and 53 executions: five selected Windows no-Runtime entries,
seven Linux no-Runtime entries, ten GSS-disabled PostgreSQL entries plus its
relocated consumer, and ten repetitions each of Linux startup/protection and
Windows SSPI ASan tests. Both gates hash the same 248 module/build/test inputs
before and after execution. No timing benchmark ran.

Raw passing and failed evidence, exact source and exploratory source are retained
in ignored `benchmarks/results/postgres-gss-transport-*20261008.*` archives.
Exploratory test/gate source is removed after archive verification, not retained
as extra committed tests. Earlier partial gates are not presented as successful.

Positive Windows domain/Kerberos encryption is still unqualified. GSS does not
have authenticated TLS-style EOF. Local sockets reject non-disabled encryption
instead of ignoring it, and the default stays disabled rather than libpq's
provider-dependent prefer policy. This is documented interoperability with
deliberate security differences, not exhaustive libpq parity or a security audit.

## OAuth Foundation

Token/provider ownership and the private OAUTHBEARER codec passed ad hoc probes
on Windows Debug, Linux Debug and Linux ASan before becoming permanent tests.
Ten cases / 658 assertions cover token grammar/bounds, literal wire-byte oracles,
mechanism-list/state rejection, cleansing of moved/replaced/grown credentials,
unstarted and rejected Tasks, coroutine-lambda ownership, failed/cancelled children,
Context shutdown, and 32 concurrent requests on both four-worker schedulers.
The shared-provider cases drop the public provider before completion and verify
that its retained callable is released only after all children drain.

An initial Windows probe had two failed assertions: its 5 ms operation and 1 ms
cancellation timer could expire on the same clock tick. The corrected probe keeps
the cancelled operation pending for an hour rather than relying on timer resolution.
This was a test assumption, not a library correction; the failure remains recorded.

The final sequential gate passed 43 steps / 132 selected CTest executions:
PostgreSQL and TLS in Windows/Linux Debug, Release and ASan; nine component
isolation/relocation/header entries on each platform; and the OAuth unit target
in Windows/Linux no-Runtime and Linux GSS-disabled builds. The 259 module/build/test
inputs were identical before and after the gate. Installed consumers instantiate
the token/provider API without a server. No performance benchmark ran.

Exact source and evidence are archived under the ignored
`benchmarks/results/postgres-oauth-foundation-20261008.*` paths. Scratch gate/build
source is removed after archive verification. These tests qualify the
[foundation contracts](postgres-oauth.md), not PostgreSQL OAuth authentication:
connection integration, trusted discovery/reconnect and native HTTPS device
authorization are still pending. No real OAuth login or production readiness is
claimed by this checkpoint.

## OAuth Custom-Provider Connections

The later 2026-10-08 checkpoint integrates the codec into real startup: a protected
empty-token discovery exchange, strict issuer-bound JSON metadata, separate
acquisition timeout, and a same-endpoint/security-policy reconnect. These are
custom-provider OAuth logins, not native HTTPS discovery/device authorization.

Ad hoc TLS-peer and cleansing probes passed before promotion to eight permanent
cases / 146 assertions. They cover exact trusted discovery URL spelling, scope
override including empty scope, duplicate decoded JSON keys, invalid UTF8/types,
structure/input bounds, early/repeated/mixed authentication messages, required
channel binding, plaintext rejection, service/URI parsing, discovery socket closure
before acquisition, timeout/failure, cancellation and Context shutdown. An unused
second-host sentinel verifies that post-connect errors do not start failover.
The initial cancellation probe incorrectly translated an observed cancellation
into its own `bad_message`; it was corrected to propagate cancellation. This
failed probe and the correction are retained in the diagnostic evidence.

Real PostgreSQL 18.6 uses an explicitly owned disposable Linux cluster and a tiny
test-only opaque-token validator. Context and blocking clients query successfully;
Context reset reacquires credentials; 32 independent connect/reset jobs run on
four workers under both schedulers. Each Runtime-enabled fixture observes 131
provider calls and an independent libpq control succeeds (18.4 Windows, 18.6
Linux). Windows Debug and ASan frontends use IOCP against that Linux backend;
Linux Debug/Release/ASan frontends use io_uring. The no-Runtime Linux fixture also
passes, with three calls. No existing cluster is stopped or altered. This does
not qualify a Windows PostgreSQL validator backend or a real JWT/identity provider.

The sequential final gate passed **132 CTest executions**: PostgreSQL in all six
main Windows/Linux Debug/Release/ASan configurations, TLS and nine component
packaging entries per platform, plus focused no-Runtime and GSS-disabled tests.
All **288 module/build/test inputs** and the user-staged index were identical
before/after the gate. Native Windows Debug/ASan real-server controls also pass
against the final source. Weave and its fixtures, not third-party libraries or
the PostgreSQL backend, are sanitizer-instrumented.

The owned server fixture is opt-in through `WEAVE_POSTGRES_OAUTH_SERVER_TESTS`,
`WEAVE_POSTGRES_SERVER_BIN` and `WEAVE_POSTGRES_OAUTH_SERVER_INCLUDE`. PostgreSQL 18
server headers and libpq are test dependencies only. Library-only consumers and
ordinary tests need no PostgreSQL server headers or validator. That checkpoint's
unmodified pinned yyjson was compiled privately as C++ with no exported yyjson API;
its MIT license is installed with the module.

Exact source, gate results and scratch diagnostics are archived under ignored
`benchmarks/results/postgres-oauth-discovery-20261008.*`. Scratch sources/scripts
are removed after archive verification. No performance benchmark ran: the libpq
controls check functionality and do not use matched connection counts for timing.
Native HTTPS/device authorization, cached-token fast paths and combined native
GSS-encrypted OAuth qualification remain open. These gates are not a security audit
or production/full-libpq-parity claim.

## OAuth HTTPS Foundation

The next 2026-10-08 checkpoint adds a private HTTPS transport over Weave TCP/TLS,
strict llhttp 9.4.3 response framing and literal uriparser 1.0.2 URLs. The latter
also replaces the handwritten issuer-URI validator without normalizing trust
identifiers. These are privately embedded C11 engines, not a libcurl dependency,
new public HTTP module or implemented device authorization provider.

Ad hoc probes passed Windows/Linux normal and ASan builds before promotion to a
permanent integration executable: **42 protocol cases / 644 checks** with Runtime
enabled. Coverage includes fragmented headers/chunks, content-length and
authenticated EOF, informational responses, malformed/ambiguous framing,
unsupported encodings/trailers, header/body/wire bounds, literal request bytes,
CA/ALPN rejection before HTTP bytes, deadline/cancellation/Context shutdown,
credential cleansing and 32 concurrent jobs under each four-worker scheduler.
The response connection is never reused; the trailing-byte check covers bytes
already received with the frame, not every later unread TLS record.

The hostile transfer-encoding probe found a real gap: an unsupported gzip transfer
encoding was treated as EOF framing. The wrapper now explicitly permits only a
single chunked transfer encoding. Earlier probe-only issues were corrected request
length, setup-deadline/peer-lifetime and post-shutdown-admission assumptions. The
failed experiments and their resolutions are retained in the diagnostic archive,
including the abandoned attempt to compile generated llhttp C as C++.

The final sequential gate passes **139 CTest executions**: PostgreSQL in all six
Windows/Linux Debug/Release/ASan builds, TLS and nine component packaging entries
per platform, and focused OAuth tests without Runtime or Linux GSSAPI. The **242
hashed module/build/test inputs** and user-staged index are unchanged across the
gate. Private parser C symbols are prefixed on both platforms; Windows has no
unintended parser DLL exports. Installed C++ consumers import no parser targets or
native headers. A C compiler is needed only when building PostgreSQL from source;
other module-only builds remain C++-only.

Verified source, parser provenance, gate output and scratch diagnostics are
archived in ignored `benchmarks/results/postgres-oauth-https-20261008.*` files;
scratch sources/scripts are deleted afterward. No performance benchmark ran.
This qualifies the [private transport contracts](postgres-oauth.md), not native
OIDC discovery/device grants, a real identity provider, full OAuth/libpq parity
or production security. Those remain separate work and qualification gates.

## OAuth Native Device Provider

Subsequent to the HTTPS foundation gate, `OAuthProvider::device` now implements
issuer-checked metadata, application-owned device prompts/client secrets and
cancellable polling through that transport. Discovery and grant bodies are
released after decoding; private codes and prompt/credential allocations use
cleansing owners. Expiry includes grant-request latency, prompt work and polling;
stale grants are not prompted and late token responses are not accepted.

The independent Python stdlib HTTPS IdP checks form encoding, including both
Basic credentials before base64, public-client and confidential POST bodies,
issuer mismatch before credential requests, pending/slow-down/request-timeout
backoff, denial, expiry, URI fragments, prompt failure, cancellation and Context
shutdown. Timing oracles measure protocol spacing, not performance. A 32-client
wave executes on four workers under both schedulers; the fixture explicitly
admits that wave instead of using Python's default five-slot listen queue.
Owner probes also cover unstarted requests, moved/replaced secrets, prompt
ownership and cleansing, malformed/ambiguous JSON, and factory validation.

Owned PostgreSQL 18 controls perform real native device authorization followed
by SQL, reset and fresh authorization through Context and the blocking facade.
Four-worker configurations additionally run 32 sessions under each scheduler:
**132 authorizations per full-runtime control**, with independent HTTPS request
oracles. Windows Debug/ASan use an owned WSL backend; Linux Debug/Release/ASan
and the no-Runtime profile use native owned backends. The validator accepts a
test-only opaque token. No persistent cluster is changed or stopped.

The frozen final gate passed **166 CTest executions**: PostgreSQL and TLS in
Windows/Linux Debug, Release and ASan; nine component/relocation/header package
entries on each platform; and focused OAuth gates without Runtime or Linux GSS.
Each selection is checked against actual JUnit test names/counts; an empty
selection is a failure. Windows native real-server controls are supplemental to
CTest. Earlier independent normal/ASan probes on both platforms also passed all
17 device-flow controls and their real-server counterparts. Permanent tests were
added only after those exploratory controls confirmed the feature.

Evidence, source hashes, failed fixture experiments and a verified source archive
are retained in ignored `benchmarks/results/postgres-oauth-device*.json`,
`oauth-device-*.xml` and `postgres-oauth-device-qualified-20261008.zip`.
The gate verifies frozen PostgreSQL/TLS/support inputs and the unchanged Git
index. Named scratch sources/scripts are removed after archive verification.

This qualifies the documented native provider with a synthetic HTTPS IdP and
real PostgreSQL/test-only validator. It does **not** qualify a deployed IdP,
JWT validation, human/browser authorization, GSS-encrypted OAuth, full libpq
parity or production security auditing. At this checkpoint, parsed client secrets,
the explicit discovery-URI/cached-token fast path, deployment validation and matched
OAuth performance workloads remained open; no performance benchmarks were run here.

## OAuth Client-Secret Configuration

The next checkpoint implements `oauth_client_secret` in keyword/URI parsing and
explicit service loading. `OAuthOptions`, provider requests and deferred
connect/reset Tasks retain immutable shared credential snapshots. Copies share
the bytes rather than creating ordinary plaintext clones; last-owner release
cleanses the credential allocation. Parser decoding, malformed-input cleanup and
service-file growth also use cleansing storage. Caller-owned input copies remain
the caller's responsibility.

Connection/request credentials precede the native provider's optional default.
An explicit empty configuration overrides a service value but does not suppress
a programmatically configured provider default. Explicit Basic/POST may obtain
their credential from the request; missing credentials and `none` combined with
a secret fail before metadata I/O. Custom providers receive the owning snapshot.

Exploratory Windows/Linux normal and ASan controls passed before promotion to
permanent tests. Each runtime-enabled synthetic HTTPS fixture runs **24 modes**,
including Basic/POST encoding, request-over-default precedence and rejection
before metadata. Ownership probes cover parser/service replacements and malformed
input, unstarted Tasks, custom-provider failure/cancellation and 32 shared-owner
requests under both four-worker schedulers. Last-owner deallocation is observed
directly and checked for cleansing. Optional libpq 18 parser controls verify the
same decoded configuration values, not an equivalent native-IdP deployment.

Real PostgreSQL controls use parsed confidential-client credentials through
Context, the blocking facade and both Runtime schedulers, including reset:
**132 native authorizations per runtime-enabled control**. The independent HTTPS
fixture checks the actual Basic credentials. Windows Debug/ASan connect through
IOCP to an owned WSL PostgreSQL backend; Linux Debug/Release/ASan and no-Runtime
controls use owned native backends. Persistent clusters are not modified.

The sequential final gate passes **166 CTest executions**, with PostgreSQL/TLS
across all six main Windows/Linux configurations, nine package tests per platform
and focused no-Runtime/GSS-disabled profiles. Windows Debug/ASan real-server
controls are supplemental. Actual selections and JUnit names/counts are checked;
all **187 hashed module/build/test inputs** and the Git index stayed unchanged.
The first gate's stale unsupported-setting assertion is retained as failed
evidence; the corrected full gate, not a selective retry, supplies this result.

Exact source and exploratory/final evidence are archived under ignored
`benchmarks/results/postgres-oauth-client-secret*` and
`oauth-client-secret-*.xml`. Named scratch files are deleted only after ZIP entry
hash verification. No performance benchmarks ran. This does not qualify a real
IdP, JWT validation, GSS-encrypted OAuth or a security audit. Explicit discovery
URI/cached-token fast paths, deployment controls and matched OAuth performance
workloads remained open at that checkpoint.

## OAuth Explicit Discovery And Cached Tokens

Explicit OIDC/OAuth well-known URLs now derive a literal issuer identifier and
pin the exact discovery document. The synchronous, application-owned optional
cache lookup runs only after protected transport and an allowed OAUTHBEARER
challenge. A hit stays on that connection; a miss retains the existing discovery
and pinned reconnect. Lookup errors, rejected tokens, method changes and
retargeted metadata never trigger implicit refresh or host failover. Absent and
explicit-empty configured scopes remain distinguishable. Cancellation is checked
again after the callback, before emitting credentials.

Exploratory installed C++-only consumers passed **12 cases / 369 assertions** on
Windows/Linux normal and ASan builds before permanent test promotion. Controls
include seven literal well-known identities, malformed/middle-position forms,
14 cached-startup modes, cleansing after successful/rejected/cancelled requests,
reentrant provider-owner release and deferred connect destruction. Cached sessions
also run in 32-job waves under both four-worker schedulers. Callback execution is
not a cache expiry, revocation or thread-synchronization policy; those remain
application responsibilities.

Real PostgreSQL controls run Context and blocking connect/reset, then 32 sessions
under each Runtime scheduler. Per full-runtime control, **132 cache hits and 132
misses** produce **264 lookups / 132 asynchronous acquisitions**. An independent
libpq 18 explicit-discovery hook performs connect/reset with two cache lookups.
The server independently observes **399 physical connection receipts**, exactly
matching one connection per hit, two per miss, two libpq connections and the
single administrative setup connection. The no-Runtime control observes four
hits/four misses and **15 receipts**, including the same libpq/setup controls.

Native device providers also accept the optional lookup. Independent synthetic
HTTPS/real PostgreSQL controls exercise explicit-discovery misses with absent
configured scopes, successful native acquisition and reset: **132 lookups and
132 authorizations** per full-runtime control. Normal and ASan frontends passed
these controls on both platforms. Windows uses an owned WSL backend; persistent
clusters are not modified.

The frozen final matrix passed **166 CTest executions**, with an additional
verified no-Runtime real-server cache entry: **167 executions total**. Coverage
includes PostgreSQL/TLS in Windows/Linux Debug, Release and ASan; nine packaging
entries per platform; and focused optional-module profiles. Windows Debug/ASan
real-server controls are supplemental. JUnit selections/counts and all **188
module/build/test input hashes** are verified; the Git index remains unchanged.
Exploratory failures were probe setup/assertion errors, retained with their
resolutions rather than omitted from the diagnostic archive.

Source and evidence are archived under ignored
`benchmarks/results/postgres-oauth-cache*` and `oauth-cache-*.xml`. Scratch sources
and drivers are removed after archive/hash verification. No performance
benchmarks ran: the libpq and receipt counts are functional controls, not timing
claims. Combined native GSS/OAuth, real IdP deployment, expiry/revocation policy
and matched OAuth performance qualification remain separate gates; this is not
full libpq parity or a security audit.

## OAuth Over Native GSS Encryption

Linux's combined integration uses an owned MIT Kerberos realm, a disposable
PostgreSQL 18.6 cluster with TLS disabled, the private opaque-token validator,
and an independent synthetic HTTPS device-flow IdP. The administrative setup
first runs an independent libpq/psql GSS authentication/encryption control.
Weave then verifies both its selected OAuth authentication method and backend
SQL reporting `encrypted=true`, `gss_authenticated=false`, and `ssl=false`.
The statistics principal is not used as a transport identity assertion.

Custom and native device providers each run cached-hit and discovery-miss
connect/reset flows through Context, blocking connections and 32 sessions under
each four-worker Runtime scheduler. Each full-runtime phase authorizes **132**
sessions, **528 across the four phases**. Exactly one cache lookup occurs per
authorization; only custom misses invoke asynchronous acquisition, and only
native misses prompt. The IdP independently observes **132 token polls / 396
HTTPS requests**. A no-Runtime run authorizes four sessions per phase, with four
polls / twelve HTTPS requests. These are functional counts, not benchmarks.

A one-worker/one-slot GSS provider verifies that discovery retires native state
before reconnect and that reset can recycle admission. Rejected cached tokens,
invalid plaintext/mutual/channel-binding policies, a nonexistent Kerberos service,
custom-provider acquisition timeout, task cancellation and Context shutdown remain terminal and
leave the provider reusable. Cancellation of a real query waits until another
protected session observes the target backend in `PgSleep`, rather than assuming
that a fixed delay makes cancellation effective. Owning cancellation snapshots
remain usable across reset and finish.

An independent native Kerberos peer protects startup and issuer-bound discovery,
waits for the discovery socket to close, then declines GSS on reconnect. An
initial `prefer` setting must now fail as required encryption: no plaintext/TLS
startup, bearer bytes, second credential acquisition or alternative-host
connection is permitted. Both records and mutual proof use MIT GSSAPI, not
fixture cryptography.

The system libpq 18.6 connect/reset control independently confirms the same
backend security properties. Its sanitizer reproduction leaked **34,232 bytes
in 36 allocations**, rooted in the libpq credential-acquisition paths. The same
failure reproduces in a libpq-only process, while Weave's separate ASan process
exits cleanly with leak checking enabled. Final libpq functional controls are
therefore separate unsanitized executables; neither their memory safety nor a
fix to libpq is claimed. No suppression or disabled Weave leak checking is used.

Controls were confirmed in exploratory consumers before permanent test promotion.
The opt-in `weave_postgres_oauth_gss` test requires the existing Linux Kerberos,
real-server and PostgreSQL 18 OAuth-validator test options. Default consumers
gain no KDC, PostgreSQL server-header or libpq dependency. Evidence and retained
probe failures use ignored `benchmarks/results/postgres-oauth-gss*` files.

The frozen final regression matrix passed **171 CTest executions**: PostgreSQL
and TLS in Windows/Linux Debug, Release and ASan; nine packaging entries on each
platform; and focused no-Runtime/GSS-disabled profiles. The combined test passed
Linux Debug, Release, ASan and no-Runtime configurations. Actual selections,
JUnit counts and all **180 module/build/test input hashes** are verified. Staged
content remains unchanged. Source, diagnostics and evidence are archived with
verified hashes before removing the named exploratory sources and drivers.
No performance benchmarks ran.

This qualifies synthetic OAuth integration over Linux native GSS, not Windows
domain/Kerberos interoperability, a deployed IdP, JWT validation, complete
libpq parity or an independent security audit. Deployment controls and matched
OAuth performance workloads remain open.

## Real Keycloak Device Deployment (2026-10-08)

The opt-in Linux `weave_postgres_oauth_keycloak` gate now drives Weave's native
HTTPS device provider against an actual Keycloak deployment. Keycloak **26.8.0**
and Temurin **21.0.12.1+1** archives are digest-verified and freshly extracted for
each run. No system packages or trust stores are changed. Keycloak runs its
production profile with HTTPS only, backed by a fresh owned PostgreSQL **18.6**
datastore over `sslmode=verify-full`. An independent certificate-verifying user
agent follows the real login/approval forms; a password grant is not substituted
for the device flow. [Setup and scope](postgres-oauth.md#real-keycloak-gate).

The test-only PostgreSQL validator uses libcurl and json-c to call Keycloak's
authenticated HTTPS introspection endpoint. Keycloak validates the signed token;
the control also requires active status, matching issuer/client/role, PostgreSQL
audience, required scopes and unexpired credentials. Weave remains a bearer-token
client, not a JWT verifier or a production validator library. The private control
dependencies are opt-in and never exported by the installed Weave module.

Confirmed controls:

- Native Basic and Post client authentication, including reserved secret characters.
- Context and blocking connect/reset, verified backend TLS and OAuth metadata,
  plus eight simultaneous sessions on each four-worker Runtime scheduler.
- Real-token cache hits across connect/reset; syntactically valid modified
  signatures, wrong role, absent PostgreSQL audience, missing profile scope and
  expired tokens are rejected without implicit refresh.
- Bad client credentials and untrusted IdP certificates fail before a user prompt.
- Real user denial returns `permission_denied`; an unapproved grant times out,
  drains, and permits later successful acquisition on the same Context.
- **79 device prompts and 7 cache lookups** per Runtime-enabled configuration;
  **15 prompts and 7 lookups** without Runtime. Denied/pending prompts and rejected
  cache lookups are included, not mislabelled as successful authentications.

Exploratory failures remain archived as diagnostics, not passing gates. The first
discovery failure was attributed to the old test certificate's missing Authority
Key Identifier under Python's strict verification. Shared fixture certificates
now contain Subject/Authority Key Identifiers; `openssl verify -x509_strict`
and all existing TLS suites pass without weakening verification. Follow-up
fixture failures exposed Keycloak's client-audience requirement, audience-mapper
precedence and omitted profile scope during explicit realm import. The corrected
realm keeps separate introspection/PostgreSQL audiences and explicit scopes.
A setup-invalid old-binary attempt was interrupted and excluded, and a denial
assertion was corrected to the existing error contract; no library fix is claimed.

The final frozen first-party matrix passed **175 CTest executions**:

| Profile | PostgreSQL | TLS | Packaging |
| --- | ---: | ---: | ---: |
| Windows Debug | 15 | 3 | 9 |
| Windows Release | 15 | 3 | - |
| Windows ASan | 15 | 3 | - |
| Linux Debug | 25 | 4 | 9 |
| Linux Release | 25 | 4 | - |
| Linux ASan | 25 | 4 | - |
| Windows without Runtime, focused OAuth | 4 | - | - |
| Linux without Runtime, focused OAuth | 8 | - | - |
| Linux without GSS, focused OAuth | 4 | - | - |

The real Keycloak test passes Linux Debug, Release, ASan and no-Runtime profiles.
Selections/JUnit results and **282 first-party input hashes** are independently
verified. Weave's ASan leak checks stay enabled, without suppressions. This does
not instrument or qualify Keycloak, the JRE or PostgreSQL/libcurl/json-c internals.
An additional standalone promoted Debug gate is retained separately from the
175-execution matrix. No timing benchmarks ran; public API/library code is unchanged.

Evidence is in ignored `benchmarks/results/postgres-keycloak-qualified-20261008*`
and retained `postgres-keycloak-probe-*` diagnostics. The archive has **329 verified
entries**, SHA-256
`ae5ce68cb9eb99344abadcd34e612b6d97aaf8dd0376c41a4d11ee488af3781e`.
Five promoted-test deployments were verified removed after service/process-group
drain, the seven named scratch sources/drivers were deleted, and staged content
remains unchanged. The pre-existing PostgreSQL service at PID 971 is still alive.

This is pinned Linux deployment evidence, not arbitrary IdP compatibility,
Windows real-IdP or combined real-IdP/GSS qualification, refresh/revocation/key-
rotation soak, a libpq device-flow/performance comparison, FIPS qualification or
an independent security audit. Those remain separate requirements.

## Protocol Tracing Qualification (2026-10-08)

Added a separate [owning protocol observer](postgres-tracing.md), available on
Task and blocking connections. Existing connect overloads remain unchanged;
startup tracing uses an explicit trailing configuration. Registration is
synchronous and supports owning save/restore/disable. Metadata is the default;
application payloads require explicit bounded disclosure. Startup, all pre-ready
bodies, authentication and backend cancellation-key bodies remain hidden.

The confirmed prototype was promoted only after Linux/Windows imported-consumer
checks, Linux ASan and actual PostgreSQL SCRAM/mTLS probes passed. Permanent
tests cover deferred/unstarted ownership, invalid configuration, metadata/null
spans, truncation/zero caps, reset retention including failed reset, malformed
lengths and late backend keys, coalesced split-pipeline frames, duplex COPY,
cancellation/drain, four-worker schedulers and Windows shared IOCP. Actual
SCRAM and verified mTLS SCRAM-PLUS exercise Context, blocking and 16 concurrent
observed sessions per four-worker scheduler. No tracing formatter, general
SQL/row sanitization, arbitrary reentrant callbacks or libpq event hooks are claimed.

The frozen matrix passed **186 CTest executions**:

| Profile | PostgreSQL | TLS | Packaging |
| --- | ---: | ---: | ---: |
| Windows Debug | 17 | 3 | 9 |
| Windows Release | 17 | 3 | - |
| Windows ASan | 17 | 3 | - |
| Linux Debug | 28 | 4 | 9 |
| Linux Release | 28 | 4 | - |
| Linux ASan | 28 | 4 | - |
| Windows without Runtime, focused tracing/ownership | 4 | - | - |
| Linux without Runtime, focused tracing/ownership | 4 | - | - |
| Linux without GSS, focused tracing/ownership | 4 | - | - |

Selections and JUnit results are checked against **347 checked-in input hashes**,
including the private vendored parsers; installed external dependencies are not
frozen or comprehensively instrumented. Linux real-server, Kerberos/GSS and
Keycloak gates remain included in the three full profiles. Three additional
standalone native Windows Debug/Release/ASan tracing runs use owned disposable
PostgreSQL 18 clusters in WSL; they are retained separately, not added to the
CTest total. Temporary copied Windows sanitizer runtime DLLs were verified and
removed after those runs.

One initial Windows sanitizer gate was setup-invalid: the phase driver wrongly
set `detect_leaks=1`, which Windows ASan rejects before native fixture execution.
Its 20 CTest attempts (19 native/fixture failures and one passing Python tooling
test) remain diagnostic evidence, excluded from the passing matrix. Only the
harness environment changed; no networking or TLS fix is claimed. Corrected
Windows gates use ASan with `abort_on_error=1`; LeakSanitizer is unsupported there.
Linux Weave leak checks stay enabled without suppression. Earlier prototype
count assertions and tool/setup errors are separately attributed in the evidence.

Evidence is in ignored `benchmarks/results/postgres-trace-qualified-v2-20261008*`,
the retained initial `postgres-trace-qualified-20261008*` diagnostic, and
`trace-sessions-prototype-*` controls. The verified ZIP and archive receipt retain
the input snapshot, actual selections/results, prototype sources and phase
drivers before deletion. Eight named scratch sources/drivers are removed at
phase close; staged content and the pre-existing PID 971/port 55432 service are
preserved. No performance benchmarks were run for this capability change.

These are scoped correctness/ownership/packaging controls, not an independent
security audit, exhaustive race/fuzz/soak qualification or complete libpq parity.
OAuth/GSS traffic retains the observer through its shared startup machinery,
but tracing every positive Windows-domain or arbitrary real-IdP deployment is
not claimed. Authentication payload bodies remain withheld by protocol phase
and tag, not by attempting to recognize particular token contents.

## Notification Callback Qualification (2026-10-08)

Added synchronous `on_notification()` registration on Task and blocking
connections, with an owning move-only `noexcept` callback and explicit
save/restore/disable. Callbacks consume newly decoded valid notifications
instead of growing the bounded queue. Previously queued events are preserved,
not replayed; an explicit idle wait returns its owning event and also invokes
the current callback once. These [delivery contracts](postgres-notifications.md)
avoid silently swallowing the event needed to complete a wait.

The prototype was confirmed with Windows/Linux imported consumers and Linux
ASan before permanent tests were added. Controls cover move-only receiver
lifetime, deferred Tasks, old backlog, replacement at decoding time, pipeline
and duplex COPY delivery, SQL-error recovery, successful/failed reset,
reentrant registration/close/cancellation, competing waits, malformed and
oversized query/idle messages, cancellation and native completion drain.
Runtime-enabled controls include four-worker affine/stealing schedulers and
Windows shared IOCP. No hidden reader, helper thread, polling or LISTEN replay
was added; the Connection remains single-reader, not generally thread-safe.

The frozen matrix passed **185 CTest executions**:

| Profile | PostgreSQL | TLS | Packaging |
| --- | ---: | ---: | ---: |
| Windows Debug | 18 | 3 | 2 |
| Windows Release | 18 | 3 | - |
| Windows ASan | 18 | 3 | - |
| Linux Debug | 30 | 4 | 2 |
| Linux Release | 30 | 4 | - |
| Linux ASan | 30 | 4 | - |
| Windows without Runtime, focused notification/ownership | 5 | - | - |
| Linux without Runtime, focused notification/ownership | 6 | - | - |
| Linux without GSS, focused notification/ownership | 5 | - | - |

Actual selections and JUnit results match **350 checked-in compile/test input
hashes**, including private vendored parser inputs. Packaging covers relocated
PostgreSQL and all-component consumers. Installed external dependencies are
not frozen or comprehensively instrumented. Linux full profiles also retain
the real-server, Kerberos/GSS and Keycloak deployment gates. Linux Weave leak
checks remain enabled without suppression; Windows ASan runs with
`abort_on_error=1`, without unsupported LeakSanitizer settings.

Real PostgreSQL LISTEN/NOTIFY controls cover plain SCRAM and verified mTLS
SCRAM-PLUS, Context/blocking operation, reset/resubscription, source backend
IDs, retained payload copies and 16 observed sessions per four-worker
scheduler. Three additional native Windows Debug/Release/ASan runs passed
against owned disposable PostgreSQL 18 clusters in WSL; those are separate
from the CTest count. Temporary sanitizer DLLs were verified and removed.

Exploratory diagnostics remain attributed: the first probe found a missing
reentrant-cancellation guard, fixed before lock. A later competing-wait probe
incorrectly assumed `when_all` cancels siblings; its corrected control cancels
explicitly and drains both waits. An initial scratch compile used typed Tasks
with the void-only join helper; the following missing-executable attempt was
not a running test. These failures are not counted as passing qualification.

Evidence is retained in ignored
`benchmarks/results/postgres-notifications-qualified-20261008*` and
`postgres-notification-prototype-*` diagnostics. The verified archive retains
the frozen source snapshot, selections/JUnit results, phase drivers and final
prototypes before deletion of the seven named scratch sources/drivers.
Staged content and the existing PID 971/port 55432 PostgreSQL service are
preserved. No performance benchmarks ran for this ergonomic capability.

These are scoped delivery, ownership and packaging controls, not exhaustive
races, arbitrary deployment qualification, libpq event-hook parity or a
security audit. Reset retains the callback but still requires explicit LISTEN
resubscription; no extra background progress is implied.

## Lifecycle Event Development Controls (2026-10-08)

Added [owning connection/result lifecycle observers](postgres-events.md), with
synchronous registration and instance-state access. Connection and result state
are separate; accepted result registrations retain callable state without keeping
the Connection alive. Copy hooks explicitly clone/share application state, moves
transfer it, and rejected registration/create/copy instances clean up without
later destroy callbacks. Reset emits only on success; reset/destroy callback
errors cannot veto required progress or cleanup. Existing Task error semantics
and protocol behavior were not redesigned.

Imported-consumer prototypes pass Windows Debug (seven modes), Linux Debug
(five modes) and Linux ASan/LeakSanitizer (five modes, no suppressions).
They cover multiple observers, rejection, owning callable/state lifetime,
deferred session borrows, copy/move construction and assignment, old-result
retention across successful/failed resets, duplex COPY, pipelines, blocking
operation and concurrent copies from one immutable retained result. Runtime
controls include four workers with both schedulers and Windows shared IOCP.

Actual disposable PostgreSQL 18 controls pass Windows Debug, Linux Debug and
Linux ASan using SCRAM and verified mTLS SCRAM-PLUS. They cover prepared
descriptions, multiple/empty queries, batch results, pipeline row chunks,
mixed Exchange/COPY, partial query failure, reset and blocking operation,
plus 16 observed sessions per four-worker scheduler. The confirmed prototypes
were then promoted into permanent regressions. The final focused selection
passes **three CTest executions**: Windows Debug `weave_postgres_events`, and
Linux ASan `weave_postgres_events`/`weave_postgres_events_live`. An additional
standalone promoted Windows Debug real-server control passes. Earlier Windows
test output before the self-move assertion's warning cleanup is retained
separately, not counted in that final focused selection.

Retained runtime diagnostics are attributed to probe mistakes, not hidden library
fixes: the notification-only peer lacks statement ParameterDescription, so that
request moved to the actual PostgreSQL control; two real-server attempts wrongly
called input/both-only `end_copy` after COPY OUT EOF, corrected to synchronous
`copy_result`. Development notes also record compile/CLI setup errors and the
pre-execution review that removed mutation of the source during concurrent
const copies. No reproduced race is claimed for that review finding.

Evidence and the verified development archive are in ignored
`benchmarks/results/postgres-events-*`. They retain the source snapshot, focused
JUnit results, runtime diagnostics and six scratch sources/drivers before deletion.
Staged content and the existing PID 971/port 55432 service are preserved; no
performance benchmarks were run.

The broader matrix was completed separately below. These development controls
alone do not requalify every existing PostgreSQL/TLS feature or prove literal
libpq C-object lifecycle equivalence, arbitrary callback reentrancy, exhaustive
races or production/security certification.

## Lifecycle Event Qualification (2026-10-08)

Frozen source (356 selected compile/test inputs) passes **198 CTest executions**:

| Configuration | PostgreSQL/TLS entries | Packaging entries |
| --- | ---: | ---: |
| Windows Debug | 22 | 2 |
| Windows Release | 22 | - |
| Windows ASan | 22 | - |
| Linux Debug | 36 | 2 |
| Linux Release | 36 | - |
| Linux ASan/LeakSanitizer | 36 | - |
| Windows PostgreSQL without Runtime | 6 | - |
| Linux PostgreSQL without Runtime | 8 | - |
| Linux PostgreSQL with GSS disabled | 6 | - |

All affected binaries were rebuilt for ResultSet's new special members/layout.
Selections and JUnit names/counts agree; no failed or skipped entries are counted.
Packaging validates PostgreSQL-only/all-component builds, relocated consumers
and standalone public headers. Linux leak checking uses no Weave suppressions;
Windows ASan does not support LeakSanitizer. Three additional native Windows
Debug/Release/ASan lifecycle executables pass against disposable PostgreSQL 18
deployments, with verified mTLS, reset, COPY, chunks, blocking and four-worker
controls. Temporary Windows ASan runtime copies were removed after those checks.

The initial driver was interrupted during Linux Debug after a WSL restart.
Its process/session was absent, and no matching CTest/owned fixture process
remained. The original report and partial Linux log are retained. Seven partial
passes lacked a completed JUnit and are not counted; the full Linux Debug
selection was rerun. The 68 completed Windows CTests were independently checked
against their selections/JUnit, retained and not rerun or double-counted.
Source and staged-index hashes remained identical through recovery and completion.
The previously running user cluster was offline after the restart; qualification
did not start, stop or modify it and used only disposable deployments.

Evidence is in ignored `benchmarks/results/postgres-events-qualified-20261008*`;
the verified archive preserves frozen sources, completed/partial evidence and
phase drivers before deletion of their three exact scratch files. No performance
benchmarks ran. This closes this change's correctness/packaging gates, not full
PostgreSQL release qualification, arbitrary deployment/race coverage or an
independent security audit.

## Peer-Name Configuration Qualification (2026-10-08)

Linux `Options::load` now resolves `requirepeer` from explicit keyword/URI input,
service files and `PGREQUIREPEER`. A bounded reentrant forward lookup plus canonical
reverse check produces an immutable UID policy. Empty values disable enforcement;
Windows nonempty policies remain unsupported. Pure parsing and Task connect/reset
do not perform NSS work. This is explicit synchronous setup, with documented
snapshot/UID-reuse semantics, not literal libpq connection-time name resolution.

Native configuration prototypes pass Windows Debug, Linux Debug and Linux ASan.
Linux linked test-only NSS wrappers confirm bounded ERANGE growth/exhaustion,
missing names, lookup errors and canonical-name mismatch rejection without
modifying OS users, NSS configuration or production callbacks. Two real disposable
PostgreSQL 18 local-socket controls pass Linux Debug and ASan/LeakSanitizer:
connect, query, reset and retained cancellation use the loaded UID; another
existing username is rejected. The probe switches NSS wrappers to failure before
driving Context and confirms zero lookups during those operations.

After confirmation, permanent configuration/precedence/environment regressions
and a standalone Linux NSS-fault executable were added. Frozen source then passes
**27 focused CTest executions** across Windows/Linux Debug, Release and ASan:
configuration, existing protocol/local tests, credential ownership and authentication,
plus Linux peer-configuration faults. Selections/JUnit agree, with no failed/skipped
entries counted and no Linux leak suppressions. This is an affected-change gate,
not a rerun of the full lifecycle matrix above on the newer configuration code.

Development diagnostics are retained: the first live prototype omitted its log
header and did not compile; the next expected a generic permission error for a
rejected socket peer. Instrumentation confirmed the existing API returns
`pg::Error::authentication`; only the probe assertion changed. The attributed
attempts are not counted as passing controls and no transport/authentication
behavior was changed to satisfy them.

Ignored `benchmarks/results/postgres-peer-*` evidence retains final sources,
development diagnostics, JUnit and six scratch files in a verified archive before
their deletion. The staged index is unchanged. No performance benchmarks ran and
no persistent database service was started, stopped or modified. NSS snapshots
do not establish Windows peer support, arbitrary directory-service behavior,
dynamic account revocation or full PostgreSQL release qualification.

## LDAP Development And Debug Promotion

Optional native LDAP service lookup now uses Windows WinLDAP or Linux OpenLDAP
only during explicitly opted-in synchronous `Options::load`. Pure parsing and
connection/reset Tasks never acquire a hidden directory dependency or perform
LDAP I/O. The default build and per-load policy remain disabled. The
[LDAP contract](postgres-ldap.md) documents plaintext/anonymous trust, service
precedence and terminal search failures, disabled referrals/aliases, bounds,
provider timeout limits and deliberate libpq differences.

Native Windows/Linux Debug and ASan development consumers each passed 101
directory/input controls, with 29 private parser/grammar checks on each platform.
Owned OpenLDAP directories cover base/one/sub searches, first-value precedence,
fallback before search, successful-stanza termination, Unicode, multiple values,
unknown/nested settings, NUL and exact aggregate/value/count limits. A test-only
observer forwards real traffic, stalls the application search and witnesses an
error without service fallback at the configured 500 ms budget. Referral targets
receive zero connections. An injected empty binary attribute exercises native
value-array handling; it is not described as an OpenLDAP-imported empty value.
Linux ASan uses leak detection without suppressions; native provider libraries
are not themselves instrumented.

Independent reduced-module consumers on Windows/Linux pass compiled-out LDAP
rejection and parser controls, with Runtime omitted. A separate native Linux
libpq 18.6 process passes 31 functional checks for the shared scopes, precedence,
empty/multiple values, pre-search fallback and terminal malformed/search errors.
Those counts are not performance measurements or exhaustive libpq equivalence.

After confirmation, the parser, configuration and owned-directory regressions
were promoted. Frozen Debug inputs pass **11 CTest executions**: seven affected
PostgreSQL/configuration/LDAP selections and four isolated/relocated packaging
gates. The latter cover public-header probes and missing Linux LDAP dependency
rejection. The promoted Windows native executable additionally passes all 101
controls against the owned directory through WSL. Selections/JUnit agree; no
failed/skipped executions are counted. This is Debug promotion, not the full
PostgreSQL or promoted Release/ASan matrix.

Development/setup diagnostics remain attributable: private slapd dependencies
were extracted, not installed as services; slapadd rejected an empty-value LDIF,
so the empty-value case uses an explicit observer injection. The first private
parser prototype referenced an incorrect Options member; only the probe was
corrected. Permanent-test setup initially used two wrong build-target names and
omitted the standalone doctest implementation macro. Those failed attempts are
retained and uncounted; no production behavior was changed to make them pass.
The archive collector initially looked for Windows JUnit under cwd instead of
CTest's test directory; the original emitted files were located and verified
without rerunning tests.

Ignored `benchmarks/results/postgres-ldap-*` holds source fingerprints, attempts,
JUnit and verified development/promotion archives. Current confirmed sources are
archived; earlier diagnostics retain hashes/logs, not every historical source
body. Eight remaining scratch source/driver files are removed after archiving;
the confirmed directory tests now live under the PostgreSQL test module.
The staged index is unchanged, no benchmark ran, and no persistent database or
directory service was started, stopped or modified. Windows domain LDAP, native
IPv6 connectivity, hard DNS/PDU limits, deployed directory trust and broader
production qualification are not established by this checkpoint.

## LDAP Affected-Source Qualification (2026-10-08)

The promoted implementation and fixtures now pass a frozen **222 CTest
executions**: Windows Debug/Release/ASan each 23; Linux Debug/Release/ASan each 39;
Windows/Linux Debug packaging each two; Windows without Runtime 10; Linux
without Runtime and Linux without GSS each 11. Selections match the emitted
JUnit exactly, without failures, errors or skipped cases. Full profiles cover
PostgreSQL and TLS; the benchmark-tooling test uses synthetic data, not timing
measurements. Reduced profiles compile LDAP out. Linux ASan enables leak
detection without suppressions.

Three native Windows LDAP gates each pass 101 checks against owned OpenLDAP
fixtures. Three additional native Windows PostgreSQL client gates pass against
owned Linux PostgreSQL 18 primary/standby servers: SCRAM, verified TLS/mTLS,
queries, pipelines, COPY BOTH/replication, cancellation and reset, including
four-worker concurrency for both schedulers. These are Windows client gates,
not native Windows PostgreSQL server or domain LDAP qualification.

The first extra PostgreSQL driver used `--exchanges`, supplying a plugin flag
where this executable expects two SCRAM key lines. The client exited 2 while
parsing fixture input, before authentication. Source inspection identified the
mismatch; only the driver changed to `--scram-keys`. Both attempts remain in the
archive; the failed attempt is not counted. Disposable servers and directories
are drained/removed by their fixture helpers; owned Windows ASan runtime copies
are removed. Persistent services and the staged index are unchanged.

Ignored `benchmarks/results/postgres-ldap-qualified-archive-20261008.zip`
preserves 450 entries: frozen sources, commands/logs, JUnit, failed/passing extra
controls, staged-patch metadata and the three phase drivers. CRC and all content
hashes were verified. SHA-256:
`1d7778da7b8cf70b2775e8820b156d7ccfbaf5bd1a2321f13b82e8330657dd10`.
The exact scratch drivers are deleted after archiving. No benchmark ran.
This closes the affected implementation's correctness and packaging gates;
the deployment/security/full-parity limits below remain.

## Compiled System Service Directory (2026-10-08)

Explicit synchronous `Options::load` now discovers `pg_service.conf` under a
configurable compiled-in directory, after explicit/ambient system overrides.
User stanzas still win without inheriting system keys; parse/connect remain
side-effect-free with respect to configuration sources. `system_files = false`
disables compiled-in discovery independently of home-file discovery. The
[configuration contract](postgres-connections.md#configuration-loading)
documents the Weave install-directory default, explicit PostgreSQL-directory
configuration, missing-file policy and fixed path across package relocation.

Native Windows/Linux Debug development consumers exercise the real compiled
library with owned Unicode directories, precedence, source opt-outs,
missing/malformed files, and no service-file reads without a selected service.
Both platforms also pass an empty compiled-directory build and reject an
explicit relative PATH cache value. The library never executes `pg_config`.
Two failed Windows prototypes are retained: the prototype's narrow argv and
environment handling did not preserve Unicode paths. Wide-character test input
and environment setup fixed those controls; production code did not change.

After confirmation, a permanent executable compiles the exact configuration
loader with an exclusively owned test directory, without mutating system files
or introducing production injection hooks. Its source dependencies remain
private. The initial promoted target omitted its private OpenSSL include/link
dependency; that build failure is retained and uncounted, then corrected on the
test target only.

Frozen inputs pass **63 focused CTest executions**: Windows Debug/Release/ASan
and without Runtime each six; Linux Debug/Release/ASan, without Runtime and
without GSS each seven; Windows/Linux Debug packaging each two. Each of the
nine service profiles passes 35 controls. Configuration, protocol, credential,
LDAP parser/compile-out and Linux peer-name tests accompany them. JUnit matches
actual selections with no failed/error/skipped entries. Linux ASan enables leak
detection without suppressions. Packaging covers isolated and relocated
consumers and public-header boundaries.

The verified archive in ignored
`benchmarks/results/postgres-system-service-qualified-v2-20261008.zip` contains
449 entries, including frozen sources, generated-header/cache fingerprints,
JUnit, logs and phase drivers. SHA-256:
`2f79512e9a2dd681c0a830a171a56037a6c214e2f8c1c2c31541a4bfc00be59f`.
Separate development/failed-promotion archives preserve the original attempts.
All owned fixture directories are removed, and exact scratch drivers are deleted
after archiving. The staged index is unchanged; no benchmark ran and no
persistent database/directory service was modified. This is affected-source
qualification on the newer configuration code, not a rerun of the full 222-test
LDAP/TLS/PostgreSQL matrix or the remaining full release/parity gates.

## Password Development Controls (2026-10-08)

The native password API now includes synchronous verifier generation and
Task/blocking connection-policy generation and password changes. Public methods
consistently take user/password order; explicit algorithms avoid policy lookup,
while automatic MD5 needs a separate opt-in. The implementation retains one
session lease across SHOW and ALTER, builds secret mutation requests directly
in cleansing storage and suppresses their request/response trace payloads.
[Policies, ownership and cancellation limits](postgres-passwords.md).

Frozen development inputs pass **six native profiles**: Windows/Linux Debug,
Release and ASan. Each profile passes **314 real-server checks**, including
SCRAM password changes over explicit plaintext and verified mTLS/SCRAM-PLUS,
MD5 self-changes/reconnects over mTLS, rejection of the old password, safe
identifier quoting, SQL-error/aborted-transaction recovery, captured input
ownership, dropped/rejected Tasks, blocking methods and four-worker affine and
stealing runtimes with 64 sessions each. These are disposable PostgreSQL 18
deployments, not changes to the user's existing database services.

Each profile also passes **20 independent Python-peer scenarios / 140 native
checks** for explicit/automatic algorithms, UTF8/SJIS identifier bytes, policy
shape/null/binary errors, missing/unknown/updated encoding metadata, message
bounds, fully drained SQL errors, secret request/response trace suppression and
pending policy/ALTER cancellation. The peer acknowledges the query and withholds
completion; the caller observes exactly one outstanding native operation before
cancellation and equal submitted/completed counts after joining. A competing
command and reset must fail busy without reaching the peer. Allocation witnesses
require an actual owned password copy to be released and zeroed, not merely an
absence of observed dirty releases.

Separate libpq processes pass the real-server SCRAM/plaintext/mTLS and MD5
password changes, old-password rejection, quoting and aborted-policy controls:
Windows libpq 18.4 has 58 checks per profile; Linux libpq 18.6 has 59, including
its owned client-key permission setup. Independent Python PBKDF2/HMAC/SHA256/MD5
derivations verify ten SCRAM and nine MD5 Weave outputs per profile, and nine
SCRAM/nine MD5 libpq outputs. Inputs include SASLprep mappings and raw-byte
fallback cases. Random salts are checked by their derived keys, not compared
for byte identity. Linux ASan enables leak detection without suppressions;
the system/vcpkg libpq control remains outside the Weave sanitizer process.

Failed prototypes and their frozen inputs are retained separately. The original
Windows runtime timeout was reproduced as exhaustion of the test harness's
8,192 allocation slots, before password-policy checks; 32,768 slots cover the
same 64-session workload. Linux libpq's initial TLS rejection was diagnosed as
the owned fixture's client-key permissions, then fixed to 0600 without weakening
verification. The first sanitizer driver used an absent compiler-cache key;
it now reads actual generated compiler metadata. ASan subsequently identified
the allocation lookup scanning poisoned, cached dead frames before starting
the password Task. The lookup now excludes poisoned regions, still requires a
positive live-copy witness, and neither unpoisons production storage nor disables
sanitizer checks. An unsupported-encoding error was aligned with the existing
quoting APIs, and quoting moved after SHOW to use updated encoding metadata.
The final six profiles rerun the consistent public argument order and stronger
observed-pending cancellation controls.

Each verified archive contains 443 entries: exact frozen sources, development
probes/drivers, logs and staged-patch metadata, with CRC and byte/hash validation.
Final archives under ignored `benchmarks/results/`:

| Profile pair | Archive | SHA-256 |
| --- | --- | --- |
| Windows/Linux Debug | `postgres-password-development-debug-v17-20261008.zip` | `9756612c4a54c0837441640d8c836e8ffe43a6aceebea696410b8523de8480d6` |
| Windows/Linux Release | `postgres-password-development-release-v17-20261008.zip` | `7f09fa5277701c167a577c9d92bfa807ce673863eb1e8bb07bf95cc4121204b6` |
| Windows/Linux ASan | `postgres-password-development-asan-v17-20261008.zip` | `e982c049162ec382eac9230c1c0e4201163b09d4dc3e31ddf209c1a4c54a995b` |

These archives describe the development phase, not permanent CTest or full
release qualification. The subsequent promotion is recorded below. No benchmark
ran, and the staged index was unchanged.

## Password Regression Qualification (2026-10-08)

The confirmed verifier, peer and real-server probes are now permanent tests.
The allocation-copy witness is shared test support, not installed or linked into
the library. Only the 64-session password executable increases its observation
capacity to 32,768 slots; existing credential tests retain their 8,192-slot limit.
The independent libpq executable remains an explicit, off-by-default test option.
Library-only consumers neither find nor link libpq.

Frozen library/test sources pass **386 CTest entries**, with matching selection
inventories and JUnit and no failed, skipped or error entries:

| Configuration | Correctness entries |
| --- | ---: |
| Windows Debug / Release / ASan | 35 each |
| Linux Debug / Release / ASan | 53 each |
| Windows without Runtime | 33 |
| Linux without Runtime | 49 |
| Linux without Runtime or GSS | 34 |
| Windows / Linux Debug packaging | 3 each |

Every non-packaging profile passes 20 independent peer scenarios / 140 native
checks and independent derivations of ten SCRAM and nine MD5 verifiers, plus
18 native input/policy checks. Six full profiles pass 314 real-server password
checks each; three reduced profiles pass 56 each. Windows real-server controls
run separately against owned WSL PostgreSQL deployments. The GSS-disabled
profile also gets an explicit real-server control, since its CTest cache does
not enable the full server suite. Six separate libpq profiles pass their 58
Windows / 59 Linux checks and independent nine-SCRAM/nine-MD5 derivations.
The full Linux suites additionally cover native KDC, LDAP, OAuth/Keycloak and
existing PostgreSQL server controls. These are correctness tests, not timings.

Packaging covers isolated PostgreSQL/TLS/all-module builds, relocated consumers
and standalone public headers, including password API exports and linking.
Linux ASan enables leak detection without suppressions; third-party libpq runs
outside the instrumented Weave process.

Three incomplete driver records are retained, not counted as completed matrices.
The first failed because CTest truncated successful JSON output at 1,024 bytes.
The second counted only 19 peer rows because XML indentation preceded the first
JSON line. Reading the system-out element directly and raising output limits
corrected the verifier. The third rejected a preexisting Windows ASan DLL.
Its bytes match the configured compiler runtime; it is retained without overwrite
or deletion. The final driver reuses only the fully passed Debug/Release checks,
after verifying identical library/test source fingerprints and original JUnit,
and reruns ASan and the remaining profiles. No native test failure is hidden by
these harness corrections.

Ignored `benchmarks/results/postgres-password-qualified-v4-20261008.zip` seals
456 entries: frozen inputs, commands/logs, caches, JUnit, the prior qualification
archive and final audit/phase drivers. CRC and exact contents are verified.
SHA-256: `fc20eed0466cb47ede269a62f66d17ef2f724c7afb825e1896cccf2a45d41426`.
The final audit verifies all 15 password certificate directories are absent.
Server helpers drain their owned deployments; persistent services and the staged
index are unchanged. Exact scratch sources/drivers are removed after archiving.
No benchmark, commit or push ran. These affected-source gates do not replace
the remaining full-parity, deployment-soak or independent security requirements.

## Metadata Development Controls (2026-10-08)

`Connection::info()`, the blocking facade and `TlsStream::info()` now return
synchronous owning snapshots. The selected endpoint, login/database, server
version, process/protocol/transaction state and actual TLS/GSS transport are
captured without network I/O. Authenticated TLS peers include a bounded DER
leaf certificate as well as subject, issuer and SHA-256. No native handles,
passwords, tickets or backend cancellation keys are exported.
[Field meanings and lifetime contracts](postgres-metadata.md).

Before test promotion, frozen native probes passed Windows/Linux Debug, Release
and ASan. Each configuration passed 340 TLS engine checks covering both protocol
versions, absent/optional/required client identities, exact independently decoded
DER, ALPN, error/move/copy boundaries and actual stateful/ticket resumption with
required mTLS. Real-server probes passed 5,296 Windows and 5,330 Linux checks per
configuration. They exercise selected destinations, SQL-side metadata controls,
blocking calls, deferred-task/pipeline busy admission, transaction transitions,
reset and retained snapshot ownership. Both four-worker schedulers run 64 sessions,
alternating plaintext and verified mTLS. Linux additionally checks local peer
address and kernel UID. These are correctness controls, not measurements.

The initial Windows development build failed before executing tests because the
scratch probe omitted direct port/log header includes. The frozen failed attempt
is retained; corrected probes passed before promotion. Earlier passed probes had
264 engine checks; the final probes add the actual resumption controls and rerun
all six configurations. No failed timing sample is selectively removed.

Each final development archive contains 447 entries, including frozen sources,
probes/drivers and logs, with CRC and exact-content validation. Ignored outputs:

| Profile pair | Archive | SHA-256 |
| --- | --- | --- |
| Windows/Linux Debug | `postgres-metadata-development-debug-both-v4-20261008.zip` | `c8d94ff449f809cca1ff92bdaf4f7d9c38c232ae64e7f52165f60814d4f4ffa6` |
| Windows/Linux Release | `postgres-metadata-development-release-both-v4-20261008.zip` | `6859d70ab470e86f0364501fd77164305d139e3ebf2d997c0b8895b96792156c` |
| Windows/Linux ASan | `postgres-metadata-development-asan-both-v4-20261008.zip` | `eabcf01cc180c26fb79e2ba2ff8448972c756ad1152078facc22ab150b838d50` |

## Metadata Regression Qualification (2026-10-08)

The confirmed native probes are now permanent `weave_tls_metadata` and
`weave_postgres_metadata_live` tests. The real-server test is explicitly opt-in;
neither test introduces libpq or native headers into consumers. Packaging also
compiles the async/blocking connection and standalone TLS metadata APIs.

Frozen library/test inputs pass **399 CTest entries**, with exact nonempty
selection inventories, matching JUnit and no failed, skipped or error entries:

| Configuration | Correctness entries |
| --- | ---: |
| Windows Debug / Release / ASan | 36 each |
| Linux Debug / Release / ASan | 55 each |
| Windows without Runtime | 34 |
| Linux without Runtime | 51 |
| Linux without Runtime or GSS | 35 |
| Windows / Linux Debug packaging | 3 each |

All nine non-packaging profiles pass the 340 native TLS checks. Real-server
metadata controls pass 5,296 Windows / 5,330 Linux checks in full profiles and
174 Windows / 208 Linux checks without Runtime. Windows server controls run
separately against owned WSL deployments. The GSS-disabled profile receives an
explicit server control because its cache disables the wider server suite.
Full Linux suites also run the existing native KDC, LDAP, OAuth/Keycloak and
PostgreSQL controls. Linux ASan enables leak detection without Weave suppressions;
third-party binaries are not all sanitizer-instrumented.

The ignored `postgres-metadata-qualified-v1-20261008.zip` contains 450 entries:
frozen sources, caches, commands/logs, selection inventories, JUnit, drivers and
staged-patch provenance. SHA-256:
`b7b60b6de2c5eff01203cb79aa009ad891b278b43b74ce2b3b7b3f8c1ed26201`.
All 25 captured certificate fixture paths are absent after qualification.

After that gate, the TLS test fixture alone changed from automatic to static
storage so `std::exit` on an assertion still runs its destructor. A focused gate
checks this exact source-line delta against the sealed broad archive, verifies
every other frozen input and the index are identical, then builds and runs the
exact `weave_tls_metadata` selection in all nine profiles: **nine CTest entries**,
340 native checks each. Matching JUnit has no failure/error/skip and every owned
fixture is absent. This does not claim the entire broad matrix was rerun on the
test-only correction.

`postgres-metadata-fixture-qualified-v1-20261008.zip` seals 452 entries, including
fresh frozen source, focused evidence and the broad report. SHA-256:
`eb2d71f0ad0ddd5882a3b7bb9a422139e4d9a9e35ec27faed0a1682c32b9e138`.
Both qualification archives pass CRC and manifest hash validation. Exact scratch
sources/drivers are deleted after archiving. Persistent services and the staged
index remain unchanged. No benchmark, commit or push ran; these affected-source
gates do not replace full parity, deployment soaks or independent security review.

## Callable API Audit (2026-10-08)

The [function-by-function inventory](postgres-api-audit.md) records all 193
exported functions in installed Windows libpq 18.4 and Linux libpq 18.6:
187 in `libpq-fe.h` and six in `libpq-events.h`. Both platforms' headers are
byte-identical. The audit checks the sealed header fingerprints, extracts these
simple extern declarations after removing comments and requires exactly one
classified row per function. Three compatibility macros have separate rows.
This is a lexical inventory of the identified headers, not a general C parser,
runtime semantic verifier or proof of complete connection-keyword coverage.

All 196 rows are unique and complete against those inputs. The review finds
106 concrete mappings, 26 partial capabilities, 13 open capabilities, 40 stated
model differences and eight libpq-specific external interfaces. These numbers
are **not a completion percentage**: several mapped APIs differ deliberately,
and deployment/qualification gaps exist independently of function counts.
Missing portal description, status/diagnostic ownership and formatting, health
probes, option introspection, lookup helpers and other utilities are queued
explicitly rather than labelled complete through a broad category.

The documentation check verifies 80 existing local link targets across the
affected guides, confirms all six metadata scratch sources are absent and
checks the staged index is unchanged. It runs no build, runtime correctness
suite, service mutation or benchmark; previously sealed runtime qualification
remains the evidence for the implemented metadata, not this inventory.

Ignored `postgres-api-audit-v1-20261008.zip` seals 450 entries: frozen source,
reference headers, inventory/mappings, package-version observations, commands,
staged-patch provenance and the phase driver. CRC and manifest contents are
verified. SHA-256:
`d783fc23f46b218337a146bc09811d33af1231875f9a5da277351059ebbad3ad`.
The scratch audit driver is deleted after archiving. Full parity and production
qualification remain open.

## Portal Description Development (2026-10-08)

Standalone `Connection::describe_portal` and its blocking facade now issue
Describe(P) + Sync through the native engine. They own the portal name and
acquire a Connection borrow before initial suspension; no Pipeline is needed
for one inspection. The response path requires one RowDescription or empty
NoData and preserves existing error, cancellation and transport semantics.

Final development probes pass Windows/Linux Debug, Release and ASan before
permanent-test promotion. Each full native profile passes 9,541 real-server
checks: named/unnamed text and binary portals, SQL cursor description versus
FETCH formats, utility NoData, missing-portal SQLSTATE/rollback, nonexecution,
metadata ownership and deferred borrowing. Four-worker affine/stealing controls
run 64 sessions each, alternating plaintext and verified mTLS/SCRAM-PLUS.
Independent native libpq 18.4 Windows / 18.6 Linux controls pass 71 / 72 checks;
Linux's additional check establishes private client-key file permissions.

Hostile peers cover missing/duplicate descriptors, invalid NoData bodies,
unexpected parameter/data/command/completion messages, malformed columns,
resource limits and cancellation while a native receive is pending. Normal,
blocking, deferred-task and Pipeline admission controls also pass. Windows
adds shared IOCP affine/stealing profiles. No performance measurement is made.

Failed exploratory attempts are retained and attributed, not discarded:

- v1 and v6 used incorrect Windows/WSL executable path forms; the affected
  real-server native processes did not execute.
- v2/v3 expected binary metadata for SQL DECLARE BINARY. That expectation was
  wrong: PostgreSQL's descriptor is text while FETCH values are binary.
- v4's baseline accidentally loaded host libpq 16 through PATH despite an 18.4
  import library. It is not libpq 18 parity evidence. v5 privately deployed the
  matching DLLs, checked runtime version 180004 and independently confirmed the
  server's format behavior before correcting the probes.
- v7 passed all six native profiles; v8 then added binary unnamed portals and
  utility NoData controls and reran all six before promotion.

Each final development archive seals 453 entries with frozen source, drivers,
logs, CRC and exact-content validation. Ignored evidence:

| Profile pair | Archive | SHA-256 |
| --- | --- | --- |
| Windows/Linux Debug | `postgres-description-development-debug-both-v8-20261008.zip` | `2bdfef4bd0d21c810edda7d6bea0876a6fedeccaa5e8aec78551bc30b6a42281` |
| Windows/Linux Release | `postgres-description-development-release-both-v8-20261008.zip` | `129e2adc5184ba5b07a3d9d591e48cc67ad2610a7335e3633885233424a2632b` |
| Windows/Linux ASan | `postgres-description-development-asan-both-v8-20261008.zip` | `6a097fe202d2c11f3d9b5ea015863410402d63f8ef1b923b32a2eb733a60c6d7` |

## Portal Description Regression Qualification (2026-10-08)

Confirmed probes are permanent `weave_postgres_portal_description`,
`weave_postgres_portal_description_live` and optional independent
`weave_postgres_portal_description_libpq` controls. The baseline's
`WEAVE_POSTGRES_DESCRIPTION_LIBPQ_TESTS` option defaults off; libpq is never a
library dependency. Real-server controls remain opt-in and own their fixtures.
Packaging consumers compile/link both async and blocking description APIs.

The frozen promotion gate passes **416 CTest entries**, with exact nonempty
selection inventories, matching JUnit and no failed/error/skipped entries:

| Configuration | Correctness entries |
| --- | ---: |
| Windows Debug / Release / ASan | 37 each |
| Linux Debug / Release / ASan | 58 each |
| Windows without Runtime | 35 |
| Linux without Runtime | 54 |
| Linux without Runtime or GSS | 36 |
| Windows / Linux Debug packaging | 3 each |

Portal peer tests validate 21 Windows / 19 Linux modes with Runtime and 17
without it. Full native real-server profiles retain the 9,541 checks; reduced
profiles pass 195. Independent baselines validate runtime versions 180004 /
180006 and retain 71 / 72 checks. Windows and GSS-disabled real-server controls
run explicitly against owned WSL deployments outside CTest. The gate also
reruns core/transport/runtime/TLS/PostgreSQL regressions and existing metadata,
authentication, KDC/LDAP/OAuth controls in the configurations that enable them.
Linux ASan enables leak detection without Weave suppressions; provider packages
are not all instrumented.

`postgres-description-qualified-v1-20261008.zip` contains 458 entries: 455 frozen
source inputs, report, staged-patch provenance and manifest. The report retains
caches, commands/logs, exact selections and JUnit. CRC, manifest hashes and
frozen source/index checks pass. SHA-256:
`81f7910bf8e0a675f003e964c75dd4ff1395f40b8472d503c3ed69e1f2e4fa90`.
All 43 captured certificate fixture paths are absent. The staged index and
persistent services are unchanged; no benchmark, commit or push runs.

Post-gate changes only update documentation/rules and delete the seven exact
archived scratch sources/drivers. They do not alter the qualified library,
permanent tests or packaging code. The two portal rows in the callable inventory
now become Mapped; full capability/deployment parity remains open.

## Result Kinds And Retained Outcomes Development (2026-10-08)

`ResultSet::kind` now records actual protocol transitions for empty queries,
commands, completed tuples, descriptions, partial row chunks and pipeline
acknowledgments. Default/application-built storage is uninitialized. The kind
is published before lifecycle callbacks and preserved through copies/moves;
COPY phases and Sync barriers retain their existing separate event types.
`result_kind_name` is a synchronous nonallocating successful-kind name helper,
not a formatter for every libpq/native error status.

Ordinary async/blocking `query_outcomes`, `execute_outcome` and
`execute_prepared_outcome` reuse the native exchange engine and existing owning
Outcome model. They retain completed successes and server SQL diagnostics;
default query/execute still propagate SQL errors. Invalid, cancelled or resource
limited exchanges remain Task/Result failures. No error ResultSet/lifecycle
event is fabricated, and ReadyForQuery must drain before SQL outcomes return.
Deferred Tasks acquire the existing Connection borrow before initial suspension.

Final frozen development inputs pass Windows/Linux Debug, Release and ASan:
**23,379 real-server checks per full native profile**, plus independent native
libpq 18.4 / 18.6 controls with **93 / 94 checks**. Linux's extra baseline check
establishes private client-key permissions. Controls cover wire-order successes
before SQL error, stopping without synthetic results for later SQL, text/binary
and prepared execution, zero rows/columns, empty queries, descriptions/NoData,
fetch suspension/completion, batch/pipeline chunks and acknowledgments, COPY and
mixed exchanges. Copies, moves, lifecycle kind visibility and retained diagnostics
are checked across reuse/reset/finish. Four-worker affine/stealing schedulers
each run 32 sessions alternating plaintext and verified mTLS/SCRAM-PLUS.

Independent peers pass 19 Windows / 17 Linux modes with Runtime: deferred/Pipeline
admission, blocking and owning retention, duplicate/post-error responses, invalid
SQLSTATE/framing/ReadyForQuery, missing outcomes, unexpected COPY, native and
retained-data limits, EOF and cancellation after an error while ReadyForQuery is
still pending. Windows additionally runs shared IOCP schedulers. Extended-query
controls reject a completed result followed by another SQL error rather than
returning two outcomes for one operation.

v1's initial Windows development control passed. v2 passed Windows but GCC 15
hit an internal compiler error in the Linux scratch probe's nested command
initializer list inside co_await, before Linux native execution. Naming the
command vector resolves the probe without changing library behavior. That
failed archive remains retained and attributed. v3/v4 passed strengthened
controls; v5 adds exact no-synthetic-event and post-reset retention checks and
reruns all six configurations before permanent-test promotion.

Each final development archive seals 458 entries, with identical frozen inputs
across profile pairs and validated CRC/manifest content. Ignored evidence:

| Profile pair | Archive | SHA-256 |
| --- | --- | --- |
| Windows/Linux Debug | `postgres-result-development-debug-both-v5-20261008.zip` | `98e437002a2ae91f202c55c2f8da6f712cdcff05bc40a8ebef1fc8a2dcacd45d` |
| Windows/Linux Release | `postgres-result-development-release-both-v5-20261008.zip` | `477148c055bd15a1f0e9a0c86d65d3e0bc95c334409eb223041ebe69d16ad08c` |
| Windows/Linux ASan | `postgres-result-development-asan-both-v5-20261008.zip` | `b5110338067d7c282b292d0124b1dce831232520ae5b843ac02917389411d863` |

## Result Regression Qualification (2026-10-08)

Confirmed development probes become permanent `weave_postgres_results`,
`weave_postgres_results_live` and optional `weave_postgres_results_libpq` tests.
Real-server tests remain opt-in. `WEAVE_POSTGRES_RESULT_LIBPQ_TESTS` defaults off;
the baseline privately links libpq and checks its runtime version. Public headers
and library-only consumers do not acquire a libpq dependency. Packaging compiles
all six async/blocking outcome methods and checks the exported kind-name helper.

The first promotion gate passed its 38 Windows Debug CTest entries, then failed
in the scratch evidence reader because it indexed the output dictionary with a
tuple instead of one test name. This is a driver failure, not a failed native
test or a complete matrix. `postgres-result-qualified-v1-20261008.zip` retains
that attempt (463 entries), SHA-256:
`cb5a518dd6faf8dbf8f80a4d41b63e90f7b03b80e7f72d0683bac3deebb4f80f`.

The corrected v2 gate freezes unchanged library/permanent-test/packaging code
and passes **433 CTest entries**, with exact nonempty selections, matching JUnit
and no failure/error/skipped entries:

| Configuration | Correctness entries |
| --- | ---: |
| Windows Debug / Release / ASan | 38 each |
| Linux Debug / Release / ASan | 61 each |
| Windows without Runtime | 36 |
| Linux without Runtime | 57 |
| Linux without Runtime or GSS | 37 |
| Windows / Linux Debug packaging | 3 each |

Full real-server profiles retain 23,379 native checks; reduced profiles pass
785. Peer controls pass 19 Windows / 17 Linux Runtime modes and 15 without
Runtime. Independent baselines retain the 93 / 94 checks and validated versions
180004 / 180006. Windows and GSS-disabled server controls execute explicitly
against owned WSL deployments outside CTest. The same gate reruns existing
core, transport, runtime, TLS and PostgreSQL regressions, including the earlier
portal/metadata and configured authentication/KDC/LDAP/OAuth suites.
Linux ASan enables leak detection without Weave suppressions; provider packages
are not all instrumented. No performance measurement runs.

`postgres-result-qualified-v2-20261008.zip` seals 463 entries: 460 frozen source
inputs, report, staged-patch provenance and manifest. The report retains caches,
commands/logs, exact selections and JUnit. CRC, manifest, source and index checks
pass. SHA-256:
`4821b0a444a60d2cd6c4e0c4b58b8ae71b83687661bf9a234a8ab70b88446507`.
Its 43 captured certificate paths are absent. A post-gate scan of all retained
JUnit also finds eight certificate markers from the earlier portal controls;
all **51 observed paths** are verified absent without deleting unrelated fixtures.

Post-gate edits only update rules/documentation and delete the seven exact
archived scratch sources/drivers. Qualified library/permanent-test/packaging
code, persistent services and the staged index remain unchanged. No commit,
push or benchmark runs. These gates do not establish full libpq compatibility,
production deployment coverage, exhaustive fuzzing or independent security review.

## Evidence Limits

OpenSSL, ICU, libpq and PostgreSQL packages are not all sanitizer-instrumented;
the ASan gates instrument Weave and its fixtures. Passing hostile-message cases
is not exhaustive fuzzing. No independent security audit, arbitrary encoding
compatibility, thread-safe shared Connection or complete libpq compatibility is
claimed. [Serial benchmarks](postgres-benchmarks.md) and
[concurrent benchmarks](postgres-concurrency.md) distinguish measured baselines,
noisy metrics, placement limits and unqualified CPU counters. Long deployment
soaks and release qualification remain separate requirements.

## Identifier-Aware Column Lookup (2026-10-08)

`ResultSet::column_index` performs bounded synchronous identifier normalization
and first-match lookup without connection/execution state. Success distinguishes
an index, including zero, from no match. Complete quoted tokens preserve case,
double quotes and punctuation; unquoted tokens fold only ASCII uppercase bytes.
NUL, malformed encoding, incomplete/partial quoting, invalid bare tokens and
excess input fail explicitly. Copies/moves retain usable metadata. Normalization
does not alter result kinds, rows, lifecycle state or wire decoding.

Frozen development controls pass **220,509 checks in each of six native
profiles**: Windows/Linux Debug, Release and ASan. Independent libpq 18.4 on
Windows and 18.6 on Linux controls use application-built native result metadata,
not Weave talking to itself. They cover 20,000 generated bare identifiers and
20,000 quoted names, duplicate-first behavior, case, doubled quotes, punctuation,
empty quoted metadata, resource boundaries, malformed UTF8 and representative
characters in all 42 encodings. Quoted native controls match every encoding;
five selected multibyte continuations demonstrate the deliberate difference
from libpq's C-locale byte folding. Native partial quoting is also recorded as
a deliberate difference, not advertised as identical behavior.

Exploratory failures are retained: v1 has an MSVC ambiguous comparison in the
scratch probe; v2 supplies an invalid JOHAB fixture and uses abort, requiring
termination of that exact owned probe; v3 sets unsupported Windows LeakSanitizer
options; v4 omits the configured private OpenLDAP header location from its
installed-consumer arguments. These are probe/setup failures, not passing gates
or reproduced library defects. v5 corrects the fixtures/arguments, uses ordinary
failure exit and passes the complete frozen matrix.

Permanent tests are added only after that confirmation. The promoted gate
passes **44 CTest executions**, with nonempty inventories, matching JUnit
identities/counts, and no failures/errors/skips:

| Profile | Selected Executions |
| --- | ---: |
| Windows Debug / Release / ASan | 5 each |
| Linux Debug / Release / ASan | 5 each |
| Windows no Runtime | 4 |
| Linux no Runtime | 4 |
| Linux no GSS / no Runtime | 4 |
| Windows / Linux PostgreSQL packaging | 1 each |

Selections cover PostgreSQL units, retained-result peers, interoperability and
lifecycle peers; full profiles also run the permanent native libpq lookup
control. New units include four concurrent readers of an immutable ResultSet,
without ambient Context/parser state. Packaging checks isolated builds,
relocated consumers and standalone public headers; the consumer invokes the
new method for both a present and missing column. The independent controls are
opt-in under `WEAVE_POSTGRES_RESULT_LIBPQ_TESTS`; no libpq dependency reaches
the library or default consumer build. Runtime libpq versions must match the
configured header version, preventing accidental loading of another native DLL.

Windows ASan passes without unsupported leak detection; Linux ASan enables
LeakSanitizer with no Weave suppressions. Native libpq/provider packages are not
all sanitizer-instrumented. The preexisting Windows compiler-runtime DLL is
hash-checked unchanged; neither the index nor protected services are changed.
No fixtures/services are introduced by lookup itself, and no timing benchmarks
are rerun.

Sealed ignored evidence:

| Archive | SHA256 |
| --- | --- |
| `postgres-column-development-v5-20261008.zip` | `c92145994d187a2c95380e86655d7f4dc400765331709168ccce5727b3c83747` |
| `postgres-column-qualified-v1-20261008.zip` | `1ecbfe98a9352f7c4c61ab3d2abc44435b71903c6f57ac3f17bd11f30fb36ce9` |

Both archives retain exact frozen inputs, commands/logs, the preserved staged
patch and a hash manifest; promotion also retains actual inventories, JUnit and
configuration snapshots. CRC and manifest hashes are verified. Exact scratch
sources/drivers are removed afterward. This closes the column-lookup audit row,
not diagnostic/status formatting, full libpq parity, exhaustive encoding/race
coverage, deployment soaks or independent security review.

## Bounded Diagnostic Formatting (2026-10-09)

`Diagnostic::format` is synchronous and owning, with all four verbosity modes,
three context policies, explicit encoding and optional caller query text. It
retains raw fields and introduces no Connection policy/state, hidden SQL copy,
logger or Task. Default terse/never avoids automatic secondary-field disclosure;
it is not sanitization. Empty diagnostics remain empty. Validation bounds all
supplied inputs, rejects duplicate/zero fields and malformed encoding/numbers,
and fails oversized output without returning a partial message. Caret geometry
uses bounded repeated scans rather than a whole-query offset allocation.

Frozen development controls pass **39,978 checks per native profile** across
Windows/Linux Debug, Release and ASan. Each profile compares **1,331 wire cases
x 12 formatting combinations** against libpq 18.4 (Windows) or 18.6 (Linux).
An independently framed loopback backend delivers actual ErrorResponse and
NoticeResponse messages to both clients. Coverage includes ERROR/FATAL/PANIC
and NOTICE/WARNING/INFO,
localized severity/nonlocalized classification, SQLSTATE fallback, detail/hint,
internal/statement positions, source fields and missing/empty values; multiline
CR/LF/CRLF, tabs, EOF/beyond-EOF, cropped/wide/combining/supplementary text; and
representative caret geometry in all 42 encodings. Common native controls match
byte-for-byte under the C locale. This is not arbitrary Unicode-table or
deployment equivalence.

Pure controls cover default disclosure, exact/insufficient aggregate-input and
output bounds, 1-MiB payloads, maximum field counts, invalid enums, malformed
UTF8, invalid/overflowing positions, explicit empty vs absent query, owning
copies and four concurrent immutable formatters. No live Context is needed for
these controls. Generated query/diagnostic input is synthetic, not real service
or credential data.

Exploratory archives are retained: v1 compares an annotated Asio tag object
directly with its peeled HEAD commit, tripping a probe provenance assertion;
v2 omits the probe's explicit port header; v3 passes Windows Debug; v4 expands
the corpus and passes the complete six-profile matrix. The provenance check now
peels declared dependency refs to commits before comparing. No dependency source
version is changed. Disconnected updates are explicitly configured only against
already-present pinned dependencies and recorded in the development evidence.

After confirmation, permanent unit/native-peer controls and package consumers
are added. The promoted gate passes **50 CTest executions**, with exact nonempty
inventories and matching JUnit identities/counts and no failures/errors/skips:

| Profile | Selected Executions |
| --- | ---: |
| Windows Debug / Release / ASan | 6 each |
| Linux Debug / Release / ASan | 6 each |
| Windows no Runtime | 4 |
| Linux no Runtime | 4 |
| Linux no GSS / no Runtime | 4 |
| Windows / Linux PostgreSQL packaging | 1 each |

The selection covers PostgreSQL units, retained-result, interoperability and
lifecycle peers; full profiles also rerun the prior native column lookup and
new diagnostic comparison. The promoted comparison removes ambient PostgreSQL
environment settings, supplies an empty password-file source and fixes the C
locale. Its owned loopback sockets/threads and temporary binary fixtures drain
and are removed. Formatting itself creates no files/services. No real PostgreSQL
cluster, domain or protected credential source is modified. Independent libpq
remains an opt-in test-only dependency under `WEAVE_POSTGRES_RESULT_LIBPQ_TESTS`.

Packaging checks isolated builds, relocated consumers and standalone public
headers; the consumer invokes both terse and query-caret formatting. Windows
ASan has no unsupported leak-detection setting; Linux enables LeakSanitizer
without Weave suppressions. Native provider/libpq packages are not all
instrumented, and the preexisting Windows compiler-runtime DLL remains hash-
identical. Source inputs remain frozen through the gate and the staged patch
remains unchanged. No timing benchmark is rerun.

Sealed ignored evidence:

| Archive | SHA256 |
| --- | --- |
| `postgres-diagnostic-development-v4-20261009.zip` | `58e4ed006659e590b3cef42d1f91168550eea5a86e629fa0c4e43db7818d6e72` |
| `postgres-diagnostic-qualified-v1-20261009.zip` | `56f8ed3b1d4d60545e3b5fc3db83b5e59deaebbb4909bdbd688938673a3e3e80` |

Frozen source/generators, command logs and the staged patch are retained with
hash manifests; promotion also retains configuration snapshots, inventories and
JUnit. Archive CRC and member hashes are checked. Exact scratch sources/drivers
are removed afterward. Mutable connection formatting defaults, unknown-severity
classification, automatic query retention and allocator-failure recovery are
not fabricated as native behavior. Transport-error history, cross-event status
formatting, full libpq parity, exhaustive races/fuzzing, deployment soaks and
independent security review remain separate work.

## Cross-Event Status Names (2026-10-09)

Seven `status_name` overloads inspect the existing owning result/outcome,
diagnostic, COPY, pipeline and error-code models. They return static string_view
labels without creating Tasks, consulting a Connection, formatting private
payloads or allocating output. No protocol transitions, coroutine ownership,
cancellation, lifecycle events or existing result layouts change. Typed states
remain the control-flow API; another overlapping status enum is not introduced.
[Contracts and deliberate native differences](postgres-results.md#status-names).

The sealed first development matrix passes **19,831 checks per profile** on
Windows/Linux Debug, Release and ASan. It exhaustively combines result kinds,
Outcome presence flags, pipeline kinds, transaction snapshots and completion
flags; covers COPY directions/data/end, diagnostics, error categories, unknown
enum/SQLSTATE values and literal lifetime; and repeats controls on four threads.
It also checks all 13 native libpq 18 result-status constants and names using
application-created native results, against libpq 18.4 on Windows and 18.6 on
Linux. These are enum/snapshot controls, not a matched live-server or wire-status
comparison, and Weave's names intentionally differ.

Before locking the implementation, review identifies that comparing a foreign
error category against std::errc can invoke application-defined equivalence
callbacks. The final implementation only performs that comparison for standard
generic/system categories. Foreign categories remain `error`, even if their
names or equivalence callbacks impersonate PostgreSQL/cancellation. An added
callback witness confirms zero invocations. The refined development controls
pass **19,841 checks each** on Windows/Linux Debug; the final code subsequently
passes every promoted Debug/Release/ASan profile below. Both successful
development versions remain archived; the initial version is not claimed to
contain the final callback guard.

After confirmation, permanent units, optional native enum/snapshot controls and
wire-peer assertions are added. Native controls cover 16 explicit model mappings
across the 13 native statuses, with **81 checks per full profile**. Actual Weave
peers separately assert names on zero-column/empty/command results, retained SQL
errors, malformed protocol/resource/cancellation failures, duplex COPY data/end,
pipeline row chunks, SQL errors, aborted commands and acknowledged Sync.
Existing concurrent Context/Runtime controls exercise both schedulers and
Windows shared/sharded layouts. Naming does not make `copy_done` a complete COPY
success, `pipeline_sync` a transaction success, or a zero error code server health.

The frozen promoted gate passes **65 CTest executions** with exact nonempty
inventories, matching JUnit names/counts and zero CTest failures/errors/skips:

| Profile | Selected Executions |
| --- | ---: |
| Windows Debug / Release / ASan | 8 each |
| Linux Debug / Release / ASan | 8 each |
| Windows no Runtime | 5 |
| Linux no Runtime | 5 |
| Linux no GSS / no Runtime | 5 |
| Windows / Linux PostgreSQL packaging | 1 each |

Selection covers PostgreSQL units, retained-result, COPY/exchange,
interoperability and lifecycle peers; full profiles also run native column
lookup, diagnostic formatting and new status controls. All nine unit executables
report zero failed/skipped doctest cases. Packaging exercises isolated builds,
relocated consumers and standalone public headers; the consumer invokes status
naming before creating a Context. Native libpq remains test-only and explicitly
opt-in under `WEAVE_POSTGRES_RESULT_LIBPQ_TESTS`. No real PostgreSQL cluster or
protected credential source is modified.

The six diagnostic-comparison fixtures drain and report deletion; an independent
check confirms their exact Windows/Linux temporary paths are absent. Windows
ASan avoids unsupported leak-detection settings, while Linux LeakSanitizer is
enabled without Weave suppressions. Provider/libpq libraries are not all
instrumented. Frozen inputs, archive CRC/member hashes, inventory/JUnit records,
native version/check counts and the unchanged staged patch are independently
verified. The preexisting Windows ASan runtime DLL remains hash-identical.

Sealed ignored evidence:

| Archive | SHA256 |
| --- | --- |
| `postgres-status-development-v1-20261009.zip` | `288aa72f4cda867567c7588f658b11b4600c33c544e9abc6676e32ed05842f14` |
| `postgres-status-development-v2-20261009.zip` | `4889c8ea6220e43fe8e1cb91c31e1e80a1f695d4f08d28dab03f4860bced1bdd` |
| `postgres-status-qualified-v1-20261009.zip` | `be70315c472217ed277748aa0d68fbe12415eb7b62e4420a89cf1e8cbe42fcfe` |

Evidence retains exact sources/generators, staged patch and command logs; the
promoted archive includes configuration snapshots, inventories and JUnit.
Scratch sources/drivers are removed afterward. No benchmark, staging, commit
or push is performed. The callable audit now has 114 Mapped, 20 Partial, nine
Open, 42 Different and eight External rows, not a completion percentage.
Transport/startup error history, server-health reporting, remaining libpq gaps
and full deployment/release/security qualification remain separate work.

## Target-Session Diagnostic Retention (2026-10-09)

Connection factories previously retained startup server diagnostics but lost
diagnostics from a failed target-session metadata query. The implementation now
captures that query through `as_result`, copies its owning Diagnostic before
destroying the failed connection and propagates the original error code. An
earlier SQL diagnostic remains available if protocol failure or cancellation
subsequently wins. Successful checks, rejected targets, ordinary `any` selection
and retry policy are unchanged; no public API or wrapper Task is introduced.

An independently framed loopback backend confirms SQL fields, owning copies,
formatting after factory failure, malformed responses, EOF, cancellation and
target selection. The pre-fix installed Windows Debug control reproduces the
loss; exploratory controls then pass across Windows/Linux Debug, Release and
ASan, including blocking, Context and both four-worker Runtime schedulers.
Each Runtime case uses 16 simultaneous roots. Native libpq 18.4/18.6 controls
retain an intentional difference: libpq tries the next host after the target
query's SQL error, while Weave treats a connected SQL error as terminal. Both
try the next host when the target check succeeds but rejects the session.

The frozen promoted gate passes 80 CTest executions: ten in each Windows/Linux
Debug, Release and ASan profile; six in Windows/Linux no-Runtime and Linux
no-GSS profiles; and one PostgreSQL packaging gate on each platform. The new
peer runs 34 controls with Runtime or 16 without; the optional native peer runs
four. Tests explicitly check completion, connection counts and peer cleanup.
Native libpq is test-only and opt-in, never a consumer dependency.

Archive CRC/member hashes, frozen inputs, nonempty exact inventories and all
80 JUnit outcomes are independently checked. CTest truncates some successful
per-case JUnit output, including Linux native controls; full exploratory logs
are retained separately rather than claiming complete promoted per-case JSON.
Six existing diagnostic fixtures are independently confirmed absent. The
staged patch and protected Windows ASan DLL remain unchanged. Linux ASan uses
LeakSanitizer without Weave suppressions; native providers are not all
instrumented. No benchmark, staging, commit or push is performed.

Sealed ignored evidence:

| Archive | SHA256 |
| --- | --- |
| `postgres-target-diagnostic-development-v1-20261009.zip` | `ed789c35a08a9cd9556d32e54dc530d999b16c661b3af42077fe81059b4d9c7c` |
| `postgres-target-diagnostic-development-v2-20261009.zip` | `a72bfc1e7d4911c98dbdc19c357f10bb88ad548db58878eabdb4b6e50618f94f` |
| `postgres-target-diagnostic-development-v3-20261009.zip` | `8a994d39ea00e429647e7cef6d30452cc2d3d1381ea1d4a490f3b72f68a8d7fe` |
| `postgres-target-diagnostic-qualified-v1-20261009.zip` | `89e67ad483be6227ef89eb3f9ff9cf53d39ebf27bb8a3e805111bbcc01a72ab6` |

Failed exploratory versions retain the probe-only array-deduction failure and
the incorrect initial native retry-count assumption. Scratch sources/drivers
are removed after sealing. Complete transport/startup error history remains
Partial; this diagnostic-loss fix does not change callable inventory counts or
complete deployment, security or release qualification.

## Server Availability Development Checkpoint (2026-10-09)

The initial `Task<ServerStatus> ping(Options)` and synchronous
`Result<ServerStatus> ping_blocking(Options)` share existing transport/startup
machinery. A validated initial authentication request provides availability
evidence without sending PostgreSQL credentials, invoking OAuth or authenticating
a session. Valid server SQL errors classify availability too, with `57P03`
distinguished as rejecting. Classification verifies the actual SQL error category;
malformed SQLSTATE text must not manufacture availability from a protocol error.
Known security/protocol/resource/cancellation failures retain Task errors.

Six frozen exploratory profiles pass: Windows/Linux Debug, Release and ASan.
Each runs 198 Weave and 20 native libpq controls, totaling 1,308 process controls.
Weave covers Context, blocking and four-worker affine/stealing runtimes, with
16 simultaneous roots per Runtime case. Independent peers check packet framing,
credential-response absence, authentication shapes, SQL errors, EOF, partial
frames, timeouts, cancellation, dropped unstarted Tasks, invalid options,
connection refusal and unreachable-first-host retry. TLS 1.2/1.3 and mTLS controls
include untrusted/expired certificates, TLS refusal and required-client-certificate
denial. Native Windows libpq 18.4 and Linux 18.6 controls use the same peers.

Failed development and focused diagnostic evidence is retained. The original
200 ms TLS-refusal assertion could lose to the connection deadline under Windows
ASan concurrency; the focused run records `no_response`, not an authentication
success. Secure-channel controls now use a three-second budget and explicit
fixture trust. A missing-client-certificate test wrongly required the Weave TLS
category even when a transport reset yielded `no_response`; Windows Debug abort
handling obscured the failed assertion as a process timeout. Focused fail-fast
controls record the actual result. Tests now allow the corresponding transport
refusal or the exact native certificate-required alert, never accepting status.
Finally, Linux libpq rejected the generated client key's group/world permissions;
an independent native connection records that exact error. Only owned fixture
keys are then restricted to `0600`. No TLS production implementation is altered
to accommodate those test assumptions.

Archive CRC/member hashes, frozen inputs and all six 218-control inventories are
independently checked. All control exit codes, peer errors and connection counts
match expectations. Six exact certificate fixture directories are independently
confirmed absent. The staged patch and protected Windows ASan DLL remain
unchanged. Linux LeakSanitizer is enabled without Weave suppressions; native
provider libraries are not all instrumented. No benchmark, staging, commit or
push is performed.

Sealed ignored evidence:

| Archive | SHA256 |
| --- | --- |
| `postgres-health-development-v1-20261009.zip` | `bdb5c8bd2932420f66c1b3ba52db49b9a18e85d177a91232f4b833c1a7310d46` |
| `postgres-health-development-v2-20261009.zip` | `9b94e3c17b42542f804158ab0cf7d70c6e7802c56b86485260502bb0d8bb08d6` |
| `postgres-health-development-v3-20261009.zip` | `ac64d75312c096eb25bf5529c493775ca93b33f0085dd59a3f36c8f56790313f` |
| `postgres-health-development-v4-20261009.zip` | `b8e019b486caf1f2d14398fd025f18cafe2815c4d217da18b97ace9e18b67274` |
| `postgres-health-tls-diagnostic-v1-20261009.zip` | `a10c7c75e69beeab40bcd22bb87180a49be2a0a7117e18cbc4a1ac4f1e1196f6` |
| `postgres-health-tls-diagnostic-v2-20261009.zip` | `c124f3c06257aff89d22d3de0245523b3fcca1859f47cf9d660057b99a1dd9cc` |
| `postgres-health-tls-diagnostic-v3-20261009.zip` | `e9988ffd88f1152be36dff8243482c6302b8027e6c2959860eadba969c47ad69` |
| `postgres-health-tls-diagnostic-v4-20261009.zip` | `cc87af6d40b931e128a97e76ed737569d98795b16584334cc8344ce034f98827` |

This initial checkpoint is development evidence, not a promoted release gate.
At that snapshot, `57P03` terminated the probe instead of traversing further
configured hosts. Permanent tests waited until traversal was locked in, followed
by shared-IOCP, real-server, broader normal-startup regression, reduced-module
and packaging gates. The callable audit moved two Open rows to
Partial: 114 Mapped, 22 Partial, seven Open, 42 Different and eight External.
Full error history, metadata, remaining parity and deployment/security/release
requirements remain open. Scratch sources/drivers are removed after sealing.

## Server Availability Traversal And Regression Gate (2026-10-09)

The probe now traverses recovering hosts after `57P03`, connection refusal,
ordinary timeouts and disconnected transports. If no host accepts, the final
attempt determines rejecting/no_response; a previous `57P03` does not survive
a subsequent unreachable endpoint. Recognized certificate, protocol, resource
and cancellation failures remain terminal. Connected opted-in GSS failures do
not acquire weaker fallback behavior. Ordinary connect/reset policies are unchanged.

Frozen exploration passes 1,866 independent process controls: 329 per Windows
profile and 293 per Linux profile, across Debug, Release and ASan. Each includes
28 matched native libpq controls, using Windows 18.4 or Linux 18.6. Weave exercises
Context, blocking and four-worker affine/stealing runtimes with 16 concurrent
roots; Windows additionally exercises both schedulers with shared IOCP. Controls
cover all-rejecting hosts, rejection followed by acceptance/refusal/EOF, EOF and
timeout recovery, random host order, cancellation during the second attempt,
TLS recovery and certificate/protocol/resource/TLS-refusal errors that must not
fall through to an accepting second endpoint. An explicitly configured OAuth
provider is never invoked when the protected peer advertises OAUTHBEARER.

An additional isolated credential-allocation executable passes six lifetime/
cleanup controls per profile: unstarted Task destruction, async/blocking validation,
pre-start cancellation, and rejected Context/Runtime submissions. Owned passwords,
per-host passwords, private-key passphrases and SCRAM keys are observed before
release. These witnesses do not claim to erase caller copies or third-party buffers.

The failed v5 run preserves a wrong native expectation: libpq stops on startup
EOF and does not visit the accepting second endpoint. All 273 Weave controls in
that Windows Debug run passed. The corrected native control explicitly expects
no_response and one endpoint; Weave's probe-only retry is a documented difference,
not a hidden claim of identical traversal. No production change was made to
accommodate that native result. All six exploratory certificate fixture directories
are independently confirmed absent; archive CRC/member hashes, full control
inventories, exits, endpoint counts and frozen inputs are independently verified.

After implementation lock-in, permanent availability and credential regressions
pass a fresh **104-entry CTest gate**: six full 13-entry selections, three reduced
eight-entry selections (Windows/Linux without Runtime and Linux without GSS),
and two PostgreSQL component-package selections. The full selections include
normal startup/interoperability, results, events, exchanges, target diagnostics,
credentials and native result/diagnostic/status/availability controls. Packaging
checks isolated builds, relocated consumers and self-contained public headers.
All 11 nonempty inventories exactly match JUnit names, with no failure, error or
skip. CTest's successful-output threshold truncates per-control JSON; the complete
exploratory records are retained separately, not inferred from truncated JUnit.

Production source remains identical between passing exploration and the promoted
gate. Frozen inputs, staged patch and protected Windows ASan DLL are unchanged.
Linux LeakSanitizer remains enabled without Weave suppressions; native dependencies
are not all instrumented. Scratch sources and drivers are deleted after sealing.
No benchmark, staging, commit or push is performed.

| Archive | SHA256 |
| --- | --- |
| `postgres-health-development-v5-20261009.zip` (failed native expectation) | `c489f4e9f803c0a256bf8fe9fc53ae486a3b41c2c1d138a66e45a45ca4130921` |
| `postgres-health-development-v6-20261009.zip` | `e6bc2728a268289f43ff596505ee1bee9e1dae3ea20ad3763bc331eb257950f0` |
| `postgres-health-qualified-v1-20261009.zip` | `980ce205caee1bd0b51b125d8fa87d1408b157bf3ad175167d0a1761040cbeca` |

Dedicated real-PostgreSQL and protected-GSS probe controls remain to qualify.
Availability remains Partial in the callable audit, whose counts are unchanged:
114 Mapped, 22 Partial, seven Open, 42 Different and eight External. This selected
regression gate is not independent security review, complete libpq parity or
deployment/release certification. Error history, metadata and the broader work
queue remain open.

## Real-Server And Protected-GSS Availability (2026-10-09)

Dedicated real PostgreSQL 18 availability controls are now permanent. They use
owned disposable clusters, certificates and, on Linux, an isolated Kerberos
realm/cache/keytab. They never alter the system cluster, existing development
cluster or an ambient Kerberos cache. Windows clients exercise IOCP against the
disposable WSL backend; this is not native Windows PostgreSQL or domain qualification.

The frozen promotion passes **108 CTest entries**: three Windows 13-entry and
three Linux 14-entry full selections, three reduced selections of 8/9/8 entries,
and two component-package selections. Every nonempty inventory matches JUnit
names without failures, errors or skips. Isolated builds, relocated consumers
and self-contained public-header probes pass. Runtime and native comparisons
remain optional; no libpq dependency is added to the Weave library.

Complete separately retained records pass **749 real-server process controls**
(653 Weave, 96 native) and **2,259 independent wire controls** (2,091 Weave, 168
native). The real-server full matrix has 102 controls per Windows profile and
116 per Linux profile; reduced matrices have 29/33/33 controls. Weave covers
Context, blocking and both four-worker schedulers with 16 concurrent roots;
Windows also covers both shared-IOCP schedulers. The wire replay replaces the
older exploratory provider evidence described below; successful truncated
JUnit output is not the source of these full control counts.

Real controls cover SCRAM/MD5/cleartext challenges without credential replies,
verified TLS, client identities, missing identities, untrusted certificates,
incorrect roles/databases, HBA denial, refusal/recovery and unstarted/pre-cancelled
Tasks. Linux additionally checks matching and incorrect kernel peer UIDs.
Protected-GSS controls cover require/prefer, missing caches, incorrect service
names, captured default-cache identity after environment mutation, repeated
one-worker/one-slot provider reuse, and cancellation during native context work,
record wrap and record unwrap. Deadline/cancellation controls observe entry into
the delayed native work and require cleanup draining, not premature frame release.
GSS authentication challenges never call the native authentication operation.
Security/provider failures never connect to the independently observed second host.

The backend logs are nonvacuous controls: all expected trust/local/GSS-authorized
sessions are counted, setup SQL is checked independently, and client-certificate
denial must actually occur. No probe executes a query or answers a PostgreSQL
authentication challenge. Trust rules or an authenticated GSS transport can
nevertheless authorize a session implicitly. A missing HBA-required client
certificate produces `28000`, proving accepting availability but not a permitted
login. These distinctions are documented in the public availability contract.

### Dependency And Native Sanitizer Findings

Global MSBuild vcpkg integration copied OpenSSL 3.6.0 DLLs into separately created
standalone exploratory consumers despite their configured 3.6.5 dependency.
Fresh-directory repetition confirmed the automatic copy; disabling integration
matches the existing root/package CMake rule. Test executables now compare the
loaded OpenSSL version with their headers. The earlier
`postgres-health-development-v6-20261009.zip` Windows exploration therefore used
3.6.0, not the intended patched-provider profile. Its results remain historical
development evidence, not current 3.6.5 qualification. The root 104-entry gate
used the intended provider; the fresh wire replay now supplies complete records
against Windows 3.6.5 and Linux 3.5.5, with runtime DLL hashes checked independently.

The pinned Linux libpq 18.6/GSS baseline leaks in an isolated instrumented process:
plain GSS authentication reports 17,404 bytes in 26 allocations on two independent
fixtures; required GSS encryption reports 17,116 bytes in 18 allocations.
A forwarding-only credential audit observes two successful acquisitions and one
release in both paths. The [18.6 authentication source](https://github.com/postgres/postgres/blob/REL_18_6/src/interfaces/libpq/fe-auth.c)
reacquires during continuation, while the
[connection source](https://github.com/postgres/postgres/blob/REL_18_6/src/interfaces/libpq/fe-connect.c)
acquires in both required-encryption preflight and method selection. Overwriting
an earlier credential is the source-supported explanation, not a claim about
other provider versions or Windows. No Weave provider is constructed in this
native baseline branch.

All 97 Weave Linux ASan controls pass independently with LeakSanitizer enabled.
The permanent native functional helper links libpq/OpenSSL only and intentionally
does not inherit Weave's sanitizer instrumentation. Binary dependency and build
option inspection confirms that boundary. Native functional comparisons pass;
the retained native sanitizer failures are not reported as clean sanitizer gates.
Weave's own ASan/LeakSanitizer checks remain enabled without suppressions. Native
dependencies themselves are not all instrumented.

### Retained Failures And Evidence

Exploratory v1/v2 stop on the unexpected dependency copy before server controls;
v3 exposes a local-socket fixture argument mistakenly containing the Kerberos
cache path; v4 exposes an invalid two-worker/one-capacity test configuration;
v5 stops on the native baseline leak. Diagnostic v1 incorrectly relied on a
destructor printing before LeakSanitizer abort; v2 wrongly expected the native
encrypted path to be leak-free. Diagnostic v3 confirms those negative controls
and the complete Weave ASan matrix. No production behavior changes accommodate
these fixture or baseline findings.

Promotion v1 passes all six full profiles but exposes a missing native-helper
build dependency in the reduced Linux test invocation. The explicit test-target
dependency fixes that; v2 passes the complete 108-entry promotion. Afterwards,
a Windows-to-WSL path-reporting correction moves certificate-directory recording
after path conversion. Earlier cleanup checks used the correct converted path,
but the reported directory was `.`. A frozen four-profile Windows replay passes
another 335 real controls and independently confirms all eight exact cluster/
certificate directories absent. Linux directory records were already correct.
The correction changes test evidence metadata only, not library code or behavior.

An independent verifier checks every archive CRC/member digest, inventories,
process exits, endpoint counts, loaded provider versions, source identity and
37 exact absent fixture paths. The staged patch, HEAD and protected Windows
ASan runtime DLL remain unchanged. The CMake/C++ inputs are identical between
promotion and path replay; the only first-party difference is that path-recording
correction. Scratch sources/drivers are deleted after sealing. No benchmark,
staging, commit, push or evidence upload is performed.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-health-live-development-v1-20261009.zip` | `1fdf52e9d3109ef85b0b4c9e66c5cdd959a0857298aa7b78e98c545f0bb21bc2` |
| `postgres-health-live-development-v2-20261009.zip` | `870e1b82683a7d5ff97b4be9fc239e92894d95e118dbb51b177cb6a8a8b7f594` |
| `postgres-health-live-development-v3-20261009.zip` | `8dcfaceaa773850c0133e498ce28b35a1999e792b55ace7e6060de708f153269` |
| `postgres-health-live-development-v4-20261009.zip` | `253e1a4892656a3e2a584bee28bcdc2f6185d4c3e49e9763843e5b4205582bb5` |
| `postgres-health-live-development-v5-20261009.zip` | `eff135ab21accf9b84157e9d857f142560c91119940cf4978156515f4a223275` |
| `postgres-health-native-diagnostic-v1-20261009.zip` | `ead91a3bbad704c64485901e6d13d6be5b7ef71964a09bc6eef909aa6952f9b0` |
| `postgres-health-native-diagnostic-v2-20261009.zip` | `8b1612f60e22e97778bd50f466e6b2ea6de790a7e56e0ba33656582851064e12` |
| `postgres-health-native-diagnostic-v3-20261009.zip` | `5e7acedb3e5d95865ea5f8076e373baa67fc9da8066e86c73200bdb3ff6f41f5` |
| `postgres-health-live-qualified-v1-20261009.zip` | `77768eb729d0b6ac8a14d07dc875a76c8fbef9c268f28a2e5148b77b9423b4f7` |
| `postgres-health-live-qualified-v2-20261009.zip` | `f8bf7c2fbacceece3dc5e0bddf728d354995d5ba65235348d8f3b6bac112572b` |
| `postgres-health-path-replay-v1-20261009.zip` | `8dcffbcc0c88b01ad3328ea5b63acee36cae5efadb1b85108570c19195fbd72f` |
| `postgres-health-live-verification-v1-20261009.zip` | `7db176a476892651e2c6c4928830a4cbe6853f812b853e57a4c6b88cdf5ab6f1` |

Two availability rows move from Partial to Mapped: 116 Mapped, 20 Partial,
seven Open, 42 Different and eight External. This is tested-profile qualification,
not complete libpq parity, independent security review or universal production
readiness. Windows domain/Kerberos, wider OAuth deployment, replication/failover,
long-duration soaks, error history, remaining metadata/result/configuration gaps
and final matched measurements remain open.

## Bounded Connection Reports (2026-10-09)

`ConnectionReport` is an opt-in owning history for connection/reset failures,
including individual resolved addresses and failed hosts before successful
fallback. Async and blocking factories/reset share the collector. Entry/storage
limits never change retry, cancellation, security policy or the primary error.
Oversized diagnostics are marked as omitted while retaining attempt metadata
when it fits. Timeout normalization follows cancellation/native/provider drain.
Reports are borrowed until completion, independent afterwards and unchanged for
dropped/rejected unstarted Tasks. Credential owners and reset borrows are still
captured before initial suspension. Unreported connections do not allocate a
collector or add a reporting observer Task. [Contracts](postgres-diagnostics.md#connection-reports).

### Confirmed Controls And Promotion

Installed-consumer probes confirm the implementation on Windows/Linux Debug,
Release and ASan before permanent promotion. The frozen promotion passes 108
exact-inventory CTest entries: six full profiles with 12 entries each, Windows
no-runtime with 11, Linux no-runtime with 12, Linux GSS/LDAP/runtime-disabled with
11, and one PostgreSQL package gate per platform. Package gates rebuild isolated
components, relocate installations, compile standalone public headers and link
consumers exercising the report overloads. PostgreSQL gains no Runtime dependency.

CTest truncates successful JUnit stdout. A separate frozen verbose replay passes
34 entries and captures complete report-control records plus OAuth startup and
credential tests. Inventories are nonempty and match JUnit names exactly, with
no failure, error or skipped entry.

| Profile | Weave Report Processes | Native libpq Processes |
| --- | ---: | ---: |
| Windows Debug | 113 | 9 |
| Windows Release | 113 | 9 |
| Windows ASan / Release | 113 | 9 |
| Linux Debug | 94 | 9 |
| Linux Release | 94 | 9 |
| Linux ASan / Debug | 94 | 9 |
| Windows no-runtime / Debug | 44 | 0 |
| Linux no-runtime / Debug | 46 | 9 |
| Linux GSS/LDAP/runtime-disabled / Debug | 42 | 0 |

The 753 Weave processes include nine standalone formatting controls. Another
63 native processes supply matched functional controls. Fixtures independently
verify all 6,602 accepted socket counts; process records contain 79,521 explicit
successful checks, excluding separate doctest assertions. Full runtimes use four
workers, 16 concurrent roots, both schedulers and Windows shared IOCP. The OAuth/
TLS suite adds 160 concurrent scenarios per full profile (960 total), including
verified hostname rejection, discovery/reconnect failure and provider failure/
timeout. These counts are workload-specific, not a general coverage percentage.

Wire controls cover refused hosts, exact resolver order, successful fallback,
SQL rejection, malformed framing, EOF, authentication policy, TLS/GSS negotiation,
target-session recovery/rejection, cancellation after SQL errors, deadlines,
oversized diagnostics, reset success/failure and busy reset. Allocator witnesses
cover dropped/rejected factories/reset and confirm reports do not retain selected
passwords. Linux GSS failures drain real native work against a temporary missing
credential cache. Provider capacity admits all 16 roots instead of accidentally
substituting a resource-limit failure.

Windows localhost resolves at least two addresses here; WSL has one entry. Both
check exact actual order. No system hosts file/external DNS is changed to manufacture
Linux multi-address evidence. Loaded/header OpenSSL identity is checked: Windows
3.6.5 and Linux 3.5.5. Native helpers check libpq 18.4/18.6 respectively, do not link
Weave and do not inherit its sanitizer flags. Weave Linux ASan keeps LeakSanitizer
enabled without suppressions. This phase uses synthetic PostgreSQL peers, real
TLS and native missing-credential GSS work, not another positive real-server or
domain-Kerberos qualification. Earlier real-server evidence remains separate.

### Retained Failures And Evidence

Development Windows v1 misplaces relative JUnit output; v2 exposes missing
standalone no-exception doctest configuration/private includes. Versions v3/v4
identify an actual stage-classification bug: hostile OAuth responses fail before
the authentication marker. Moving the marker before valid-method policy checks
fixes classification without changing authentication behavior. Windows v7 times
out because the fixture awaits a moved-from Task rather than its JoinHandle;
corrected v8 passes. Linux v1 assumes two localhost addresses; v2 configures one
provider admission for 16 roots. Fixture fixes do not change library admission
or resolution.

Earlier developmental records have hashes without complete input copies. The v7
timeout is not recorded as a completed command by its runner. These limitations
remain explicit; failed evidence is not replaced by invented successful records.
Later confirmation/promotion/replay archives contain frozen inputs, exact commands,
logs, staged patches and independently verified ZIP CRC/member SHA256 manifests.
HEAD, the staged patch and the protected Windows ASan DLL remain unchanged. Primary
scratch sources are deleted after sealing; archived copies remain ignored evidence.
No benchmark, staging, commit, push or upload is performed.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-reports-qualified-v1-20261009.zip` | `85f17d032d6e07697cc64c4f394ba6a9543babda31b6ac11b1dd0cc8333c3829` |
| `postgres-reports-controls-v1-20261009.zip` | `b036beefe94c24d28ec21ebf69579414c147ebffa3b27dd5625f8eed5ec5ad0c` |
| `postgres-reports-development-v1-20261009.zip` | `00c7e5886935ca897440be689cedeb818fce13ff842cbbc88891269fa557084e` |

`PQerrorMessage` remains Partial: startup/reset history does not provide a complete
post-login transport/protocol ledger. The inventory remains 116 Mapped, 20 Partial,
seven Open, 42 Different and eight External. Failed-startup authentication facts,
metadata/result/configuration gaps, deployment qualification and final matched
measurements remain open. This is not full parity, an independent security review
or universal production readiness.

## Owning Session Failures (2026-10-09)

`Connection::last_failure()` and its blocking counterpart return an owning,
synchronous `Result<Failure>` after admitted operations fail. The local error
code remains distinct from retained server fields and survives transport closure.
Deferred Tasks, active callbacks and pipeline/exchange leases reject inspection
with `busy`; moved-from connections return `closed`. Existing diagnostic-clearing
boundaries clear the corresponding error, and reset retains replacement failures.
[Contracts and formatting limits](postgres-diagnostics.md#session-failures).

Existing operation guards bind only their own promise's error slot using an
immediately completing awaiter. Failed-chain cleanup destroys that guard before
its promise, so capture introduces no child-frame pointer, native-operation
wrapper Task, helper thread or logging. This does not establish performance
parity: no benchmark is run for this correctness feature.

### Corrected Qualification

Final installed-consumer confirmation passes 600 process controls across Windows
and Linux Debug, Release and ASan before regression promotion. The final permanent
gate passes **65 exact-inventory CTest entries**: seven affected suites in each of
nine profiles, plus one PostgreSQL component-package gate per platform. The suites
are core PostgreSQL tests, failures, password sessions, connection reports,
credential ownership, result envelopes and mixed exchanges. Package gates rebuild
isolated components, relocate installations, compile public headers independently
and exercise the snapshot APIs. No Runtime dependency is added to PostgreSQL.

The earlier 117-entry gate is retained as historical evidence, not qualification
of the final admission fix or shared-layout test correction. The narrower final
gate reruns the affected paths rather than claiming that every earlier integration
suite was rerun. All selections are nonempty and their exact JUnit names match;
no failed, errored or skipped case is accepted. Full verbose logs avoid truncated
successful JUnit output.

| Final Profile | Failure Processes | Accepted Connections |
| --- | ---: | ---: |
| Windows Debug | 124 | 3,099 |
| Windows Release | 124 | 3,099 |
| Windows ASan / Release | 124 | 3,099 |
| Linux Debug | 76 | 1,563 |
| Linux Release | 76 | 1,563 |
| Linux ASan / Debug | 76 | 1,563 |
| Windows no-runtime / Debug | 28 | 27 |
| Linux no-runtime / Debug | 28 | 27 |
| Linux GSS/LDAP/runtime-disabled / Debug | 28 | 27 |

The **684 processes** include nine standalone formatting controls. Independent
verification checks all **14,067 accepted connections** and **450,432 explicit
successful checks**, excluding separate doctest assertions. Full runtimes use
four workers and 32 concurrent roots for each scenario and scheduler. Windows
also configures shared IOCP under both schedulers; recorded configuration markers
are checked against the requested modes, not inferred from scenario labels.

Controls cover owning copies, deferred-task/busy boundaries, malformed error and
ReadyForQuery frames, SQL recovery and clearing, EOF, resource limits, cancellation,
row/COPY reads, an 8 MiB COPY write reset by the peer, mixed exchanges, function
calls, duplex/split pipelines, batching and encoding changes. Blocking controls
include SQL outcomes, EOF, malformed responses, preservation through later closed
calls, and moves. Password verification and password changes rejected by an idle
pipeline must not send bytes or create a retained wire failure.

Formatting controls check empty snapshots, diagnostic disclosure, malformed fields,
exact and insufficient output budgets, and diagnostic input limits. Loaded/header
OpenSSL identity is checked in every process: Windows 3.6.5 and Linux 3.5.5.
Linux ASan retains LeakSanitizer without suppressions. These are synthetic-peer
controls and package tests, not another real-server, domain-Kerberos, sustained
soak or independent security qualification. Raw server/provider text remains
potentially sensitive; capture is not sanitization.

### Retained Defects And Evidence

Development v1 uses a single-client pending-metric assertion in a concurrent
cancellation test; v3 configures a COPY message limit above its result limit.
Both fixture mistakes are corrected without changing library behavior. An early
prototype compile mistake passed a string instead of a pipeline Command; its
terminal output is not present in the sealed development records.

The v6 probe reproduces an actual admission-bookkeeping defect: password work
rejected while a pipeline owns the session incorrectly records `busy`. Moving
promise-error capture after that admission check fixes it without filtering real
errors or changing the wire/coroutine state machine. Six-profile confirmation v2
verifies the fix. Inspection then finds that scenario-prefixed modes labelled
shared IOCP were selecting sharded IOCP. Confirmation v3 corrects the selector,
checks explicit configuration markers and adds blocking failure controls before
permanent promotion. Earlier shared labels are not shared-backend evidence.

Frozen source copies, commands, caches, raw logs, JUnit inventories and staged
patches are retained in ignored archives. A separate verifier checks input hashes,
control inventories/counts, configuration markers, ZIP CRCs and every manifest
member SHA256. HEAD, the staged patch and the protected Windows ASan DLL remain
unchanged. Primary scratch sources are deleted after sealing; archived copies
remain ignored evidence. No staging, commit, push, upload or benchmark is performed.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-failures-qualified-v1-20261009.zip` | `d119eee03e60d4dd40cb9e45e39e02eae24545b01985052f7d2b6d1d51fbb1af` |
| `postgres-failures-qualified-v2-20261009.zip` | `b871223e2350602d687b10c0012aa013c008ae02aa8532eb61ce8edffc7caa23` |
| `postgres-failures-development-v1-20261009.zip` | `6065fc3729eca99f9ccb85140ea7b4c86a3bc15d6dc9b9fefb4f6172173e4a11` |
| `postgres-failures-verification-v1-20261009.zip` | `33d8c9c5d7b4c2d4a8028fa43d77e00d23ac982915af7cf60c38097421b2fa15` |

`PQerrorMessage` remains Partial: pre-admission/application errors belong to their
Task/Result rather than a persistent per-invocation message buffer. The inventory
remains 116 Mapped, 20 Partial, seven Open, 42 Different and eight External, not a
completion percentage. Authentication/session metadata, result/configuration gaps,
deployment qualification and final matched libpq measurements remain open.

## Session Parameter And Version Metadata (2026-10-09)

Both connection facades now return a synchronous owning optional string from
`parameter(name)`, preserving missing-versus-reported-empty semantics. ConnectionInfo
also owns the selected Startup `server_options` text and a numeric
`server_version_number`, alongside the existing raw version string.
[Public contracts and source-breaking getter migration](postgres-metadata.md#parameters-and-versions).
These are metadata changes only: no transport, coroutine, authentication,
cancellation or scheduling policy changes, and no performance claim.

### Final Qualification

Installed-consumer prototypes pass six Windows/Linux Debug, Release and ASan
profiles before permanent promotion: 720 Weave processes, 114 isolated native
libpq processes and 14,628 accepted fixture connections. Final permanent tests
then pass **72 exact-inventory CTest entries**, **18 separate real-server runs**
and independently verified raw metadata inventories. The CTest count includes
both PostgreSQL package gates; the direct real-server runs are not counted as
CTest entries.

| Final Profile | Weave Metadata Processes | Native libpq Processes | Accepted Metadata Connections |
| --- | ---: | ---: | ---: |
| Windows Debug | 144 | 19 | 3,206 |
| Windows Release | 144 | 19 | 3,206 |
| Windows ASan / Release | 144 | 19 | 3,206 |
| Linux Debug | 96 | 19 | 1,670 |
| Linux Release | 96 | 19 | 1,670 |
| Linux ASan / Debug | 96 | 19 | 1,670 |
| Windows no-runtime / Debug | 48 | 0 | 96 |
| Linux no-runtime / Debug | 48 | 19 | 134 |
| Linux GSS/LDAP/runtime-disabled / Debug | 48 | 0 | 96 |

The synthetic metadata selection contains **864 Weave processes**, **133 native
processes** and **14,954 accepted connections**. Independent verification checks
all 22,431 owning JSON snapshots against the expected three phases and every
engine/case pair, not just success markers. Context and blocking runs use one
root; runtime runs use four workers and 16 concurrent roots under both schedulers.
Windows additionally uses both shared-IOCP policies, with explicit configuration
markers checked against the requested mode. Reduced builds test both facades
without importing Runtime into the PostgreSQL library.

Twenty-four Weave cases cover absent/empty versions, modern and pre-10 forms,
prerelease/vendor suffixes, numeric prefixes, partial components, whitespace/sign
syntax, negative/overflow values and the signed-32-bit boundary. Nineteen defined
cases match native libpq exactly. Five negative/overflow cases are deliberately
excluded from the native oracle: Weave safely returns zero instead of copying
unsafe conversion/arithmetic behavior. This does not qualify obsolete servers.

All cases exercise missing, empty and nonempty ParameterStatus values, owning
copies across updates, effective options before/after fresh-options reset, move
and finish. Deferred Tasks, idle pipeline leases and notice callbacks reject
inspection with busy; embedded-NUL names fail without closing the session.
Native controls finish and reconnect with fresh options; they do not pretend
PQreset accepts replacement options. The native helper never links Weave or
inherits its sanitizer flags; loaded versions match configured libpq 18.4 on
Windows and 18.6 on Linux. Every Weave process checks loaded/header OpenSSL
identity; Linux ASan retains unsuppressed LeakSanitizer.

Seven affected suites run in each profile, with a separate native metadata test
in the six full profiles and Linux no-runtime build. Core, failure snapshots,
session encoding, connection reports, credential lifetime and result envelopes
are rerun rather than claiming the whole integration suite was rerun. All CTest
inventories and JUnit names match exactly, with no failure, error, skip or empty
selection accepted.

The 18 direct runs execute metadata, encoding and general live controls in all
six full profiles against fresh owned PostgreSQL 18 primary/standby fixtures.
Metadata compares the numeric snapshot with server `server_version_num`, checks
actual Startup options, plaintext and verified mTLS, moves/reset, blocking/local
operation and four-worker/64-root workloads under both schedulers. Existing
encoding, SQL_ASCII, query, pipeline, COPY/replication and cancellation checks
also pass. Fixtures drain clients and stop/delete their owned clusters and
certificate directories; unrelated PostgreSQL services/datastores are untouched.
Windows/Linux package gates rebuild isolated components, relocate consumers and
compile independent public headers plus both getter signatures.

### Retained Failure And Evidence

Confirmation v1 rejected the old session's EOF during fresh-options reset even
though the metadata values and checks passed. Existing Weave reset closes that
transport without a Terminate message. The fixture now accepts that EOF only
after the expected initial query completed and counts it separately from native
Terminate and reset-session finish. This fixes the fixture, not library behavior;
the failed record remains sealed and is not counted as final qualification.

Frozen source copies, commands, caches, full logs, exact JUnit inventories and
the staged patch are retained in ignored archives. A separate verifier checks
all metadata records, native link/sanitizer boundaries, real-run markers,
credential-directory cleanup, source hashes, ZIP CRCs and every manifest member
SHA256. HEAD, the staged patch and the protected Windows ASan DLL are unchanged.
Primary ad hoc sources/scripts are deleted after sealing. No staging, commit,
push, upload or benchmark is performed.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-metadata-confirmed-v1-20261009.zip` | `806794fce52add58ab8c1725e4c38bc7c70977ae4bab7f2ea9b5ade67829fa0d` |
| `postgres-metadata-confirmed-v2-20261009.zip` | `1695e160aad045722433209046e00e703210402f518ee692eb7d9ea278f491fa` |
| `postgres-metadata-qualified-v1-20261009.zip` | `6ffd4199cb2de100b6b1cb8e69740dbac1cef9245a0cdf4d7e3eee60f469418a` |
| `postgres-metadata-development-v1-20261009.zip` | `035ea99173fa5550eb486aa9c6fcb13d50735e50a1371969135c16e6fa8308c5` |
| `postgres-metadata-verification-v1-20261009.zip` | `8c229457895d86db16271f8067b4de5e4a5182aed48a81e92226fb0916395b70` |

The callable inventory is now 119 Mapped, 18 Partial, six Open, 42 Different and
eight External, not a completion percentage. Complete nonsecret option
introspection, failed-startup authentication facts, transaction-active/unknown
states, result/configuration gaps, broader deployment qualification and matched
libpq performance measurements remain open. Automated affected-feature gates
are not an independent security audit or a universal production-readiness claim.

## Authentication Fact Snapshots (2026-10-09)

`AuthenticationInfo` now owns the selected method, valid password-challenge and
missing-password facts, and accepted AuthenticationOK state. `ConnectionInfo`,
the latest `ConnectionReport` and each retained failed `ConnectionAttempt`
contain scalar copies. Changing `ConnectionInfo::authentication` from an enum
to this structure is source-breaking: use `.authentication.method`.

Challenge facts are captured before credential cleanup. A password request does
not prove that a password was sent or accepted, and a missing configured password
can coexist with successful SCRAM-key authentication. Authentication completion
does not mean ReadyForQuery, a healthy session or successful authorization.
Failed factories still return Task errors, not unusable Connection objects;
optional borrowed reports retain the nonsecret facts without exporting secrets.
Truncated attempt history does not discard the latest report facts.

A startup-frame guard publishes the record before the owning parent releases
its implementation/history. This does not add a wrapper coroutine, promise
handle, credential clone or allocation for the facts. Inspection remains
synchronous. These are architectural properties, not a measured performance win.

Installed-consumer confirmation passes six Windows/Linux Debug, Release and
ASan profiles before permanent promotion: 714 Weave processes, 132 isolated
native libpq processes, 8,244 accepted connections and 18 affected root CTests.
Final permanent qualification passes **128 exact-inventory CTest entries** and
**18 separate real-server runs**. The direct runs are not counted as CTests.

| Final Profile | Weave Authentication Processes | Native libpq Processes | Accepted Authentication Connections |
| --- | ---: | ---: | ---: |
| Windows Debug | 143 | 22 | 1,806 |
| Windows Release | 143 | 22 | 1,806 |
| Windows ASan / Release | 143 | 22 | 1,806 |
| Linux Debug | 95 | 22 | 942 |
| Linux Release | 95 | 22 | 942 |
| Linux ASan / Debug | 95 | 22 | 942 |
| Windows no-runtime / Debug | 47 | 0 | 53 |
| Linux no-runtime / Debug | 47 | 22 | 78 |
| Linux GSS/LDAP/runtime-disabled / Debug | 47 | 0 | 53 |

Independent verification checks **1,009 authentication processes**, **8,428
accepted connections** and **7,489 owning JSON records**, including every expected
case/engine/root multiplicity. These counts cover the authentication selection,
not every process in the broader gate. Runtime controls use four workers and
16 roots under both schedulers; Windows also tests both shared-IOCP policies.
Configuration markers are checked explicitly. Cancellation uses a joined,
Context-owned child rather than pretending native libpq exposes the same control.

Twenty-four Weave cases cover trust, denied/unknown/truncated startup, password
policy and absence, MD5 and SCRAM success/rejection/absence, supplied SCRAM keys,
unsupported/malformed mechanisms, nonce/server-proof failures, EOF, cancellation,
failure after AuthenticationOK and successful/rejected/key-based reset. Twenty-two
defined native cases match the two challenge/presence flags. Malformed SASL and
Task cancellation are excluded from that native oracle. Native partial flags on
malformed input are not promised as Weave behavior. Insecure cleartext policy and
missing MD5/SCRAM credentials can produce different wire responses; flag agreement
does not claim identical security policy or actual password transmission. This
change does not alter either existing response policy.

Snapshots remain independent across copies, moves, finish and reset, including
retained old values. Unstarted/dropped Tasks leave the supplied report untouched;
busy reset rejects without changing the usable transport. Deferred Tasks and idle
pipeline leases retain the existing admission/lifetime rules. Real-server metadata
controls compare authentication completion with actual successful startup and
exercise plaintext, verified mTLS, local/blocking operation and four-worker,
64-root sharded workloads under both schedulers.

The exact CTest inventory includes 12 affected suites in nine profiles, two
isolated native suites in seven profiles, Linux GSS startup in four profiles and
two package gates. Existing OAuth startup/device, authentication, interop, failure,
credential, result, encoding and metadata controls are rerun. Linux GSS controls
use private KDC/server fixtures; they do not qualify Windows domain deployment.
All JUnit inventories match, with no failure, error, skip or empty selection.
Linux ASan retains unsuppressed LeakSanitizer. Every Weave process checks loaded
OpenSSL against its headers: 3.6.5 on Windows and 3.5.5 on Linux. Native helpers
link libpq without Weave or its sanitizer flags and check configured/loaded
18.4 on Windows and 18.6 on Linux.

The 18 direct runs execute metadata, encoding and general live controls in all
six full profiles against fresh owned PostgreSQL 18 primary/standby fixtures.
Their existing query, pipeline, COPY/replication and cancellation checks pass.
Owned clusters, certificate directories and regular empty password files are
cleaned up; unrelated services/datastores are untouched. Package gates rebuild
isolated modules, relocated consumers and standalone public headers, including
the new metadata signatures, without introducing a public libpq dependency.

### Retained Failures And Evidence

Three failed confirmations remain retained: v1 rejected an empty runtime-DLL
copy command; v2 could not resolve the Linux installed consumer's LDAP headers;
v3 rejected libpq's warning that `/dev/null` was not a regular password file.
These were harness/build-consumer failures, not evidence of a library defect.
The fixes use explicit private runtime DLLs, the configured LDAP paths and an
owned regular empty password file. No warning is suppressed; v4 and the final
gate pass. Failed records are not counted as successful qualification.

Frozen source, commands, caches, raw logs, exact JUnit inventories and staged
patch copies are retained in ignored archives. Independent verification checks
record multiplicities, native link boundaries, real-run markers, owned-resource
cleanup, source hashes, ZIP CRCs and member SHA256s. HEAD, the staged patch and
the protected Windows ASan DLL remain unchanged. Primary ad hoc sources/scripts
are deleted after sealing. No staging, commit, push, upload or timing benchmark
is performed.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-auth-info-confirmed-v1-20261009.zip` | `a670ec97133b20b0e0258442d71d07dab0639ad00c7cc89ba90a0f7ae2084dab` |
| `postgres-auth-info-confirmed-v2-20261009.zip` | `df492be8cd4c1dc817d471f5ae4868a5c5966a5f5a5f5e527709dc1993a20edd` |
| `postgres-auth-info-confirmed-v3-20261009.zip` | `f80a440a51d13518555c4dfaae9ed28f6ef1d90589fc62c30b2c8606fed81a49` |
| `postgres-auth-info-confirmed-v4-20261009.zip` | `ac7d1ac66c32525c2d024999c11062ff32b4b285ba292b002bf225ddf3e1a747` |
| `postgres-auth-info-confirmation-verification-v1-20261009.zip` | `cdaf441d1e012cb1a22f929c4f39d3238e54112f3938f2324f29b8ff08ceeb45` |
| `postgres-auth-info-qualified-v1-20261009.zip` | `f87d75f17bc3d6c5494bad318ad0772ab79944658028034a9af06180d63e940c` |
| `postgres-auth-info-development-v1-20261009.zip` | `e2d0f1ecee1b4151557862ada7e42245194d50aa706385f8d7e75ad7e63818b9` |
| `postgres-auth-info-verification-v1-20261009.zip` | `31537b8cba54be6be52425d33dc275c62b096d53a3cb7ef03f6e38536e7cccb5` |

The callable inventory is now 121 Mapped, 17 Partial, five Open, 42 Different and
eight External, not a completion percentage. Nonsecret option introspection,
transaction-active/unknown states, result/configuration gaps, broader deployment
qualification and matched libpq performance measurements remain open. Automated
affected-feature qualification is not an independent security audit or a
universal production-readiness claim.

## Static Option Schema (2026-10-09)

`option_schema()` now returns immutable static keyword, environment,
built-in-default, support, secret and constraint descriptors. Ordinary parser
recognition and loader environment mapping use this single table. Dedicated
service-file selection and conditional legacy-SSL handling remain separate;
all previous accepted/rejected keyword classifications and environment mappings
are preserved. No credentials, live Options or opaque provider/context handles
are copied. The API performs no ambient-source reads or allocation and does not
require a Context or Runtime. [Public contract](postgres-connections.md#option-schema).

The 53 descriptors contain all 50 keywords exposed by the pinned native libpq
18.4/18.6 builds and three existing Weave aliases. Forty-one keywords have parser
entry points, three require the loader, eight remain unsupported and key-log
export remains unrecognized. Availability is not a promise that every value,
platform or provider works. Thirty-seven environment names are exposed; 35
ordinary mappings come from the shared table and two retain dedicated loader paths.
SCRAM keys are secret even where native display metadata calls them debug fields.

Installed-consumer confirmation passes Windows/Linux Debug, Release and ASan
before permanent promotion: **48 affected CTests**, **228 Weave processes** and
**six isolated native processes**. It independently compares recognition and
ordinary environment mapping against the previously qualified implementation,
exercises each descriptor's parsing classification, checks actual typed defaults
and every isolated environment mapping, and queries native descriptor inventory.
Permanent expectations are checked against all six raw confirmed inventories
before their final gate; the confirmed C++ body is preserved with an explicit
standard-library include added.

Final qualification passes **112 exact-inventory CTest entries**: 11 affected
suites in nine profiles, seven native controls, four Linux peer-configuration
controls and both Windows/Linux package gates. There are no failure, error, skip
or empty selections. Root suites cover parsing, configuration/system services,
credential lifetime, authentication, connection/failure reports, session metadata,
LDAP and OAuth startup. The nine profiles include full Debug/Release/ASan,
Windows/Linux no-runtime and Linux GSS/LDAP/runtime-disabled builds.

| Final Profile | CTests | Weave Schema Processes | Native Schema Processes |
| --- | ---: | ---: | ---: |
| Windows Debug | 12 | 39 | 1 |
| Windows Release | 12 | 39 | 1 |
| Windows ASan / Release | 12 | 39 | 1 |
| Linux Debug | 13 | 39 | 1 |
| Linux Release | 13 | 39 | 1 |
| Linux ASan / Debug | 13 | 39 | 1 |
| Windows no-runtime / Debug | 11 | 38 | 0 |
| Linux no-runtime / Debug | 13 | 39 | 1 |
| Linux GSS/LDAP/runtime-disabled / Debug | 11 | 38 | 0 |
| Windows/Linux package gates | 2 | Not counted | Not counted |

Independent verification checks all **349 Weave** and **seven native** schema
process records, including exact argument multiplicities, 365 baseline C++ guards,
53 descriptor rows, 37 environment controls and 50 native rows. Every Weave
process verifies loaded/header OpenSSL identity; raw versions are checked against
3.6.5 on Windows and 3.5.5 on Linux. Linux ASan uses unsuppressed LeakSanitizer.
Native helpers check configured/loaded libpq 18.4/18.6; project/reference and
Ninja-command inspection confirms no Weave linkage or inherited sanitizer flags.
These helpers inspect native compiled descriptor defaults, not dynamic `val`
values or credential data. Intentional default differences are retained rather
than labelled equality. Native helpers are functional controls, not a separately
sanitizer-qualified libpq implementation.

Each environment control uses isolated PG variables and owned temporary
service/password files, then verifies directory cleanup. No connection is opened
by the schema helper; existing affected network controls are not a new real-server
or TLS release qualification. Package gates rebuild isolated modules, relocated
consumers and self-contained public headers including the schema signature;
installed consumers also execute the API in all six confirmation profiles.

Confirmation v1 passes the Windows Debug suites and initial schema/native checks,
then fails because the probe expects a single-host password-file match in the
host list instead of `Options::password`. Its failed check uses abort and hits
the observation timeout. The corrected assertion and prompt failing-process exit
are test-only changes; no library behavior is changed. The failed archive remains
retained. Verification v1 also has a mistaken hardcoded 367-check expectation;
v2 derives the correct 365 count from the confirmed C++ body. Its original source
and correction are retained in the development archive, not counted as passing
evidence. The final gate is not rerun to hide either harness mistake.

Frozen source, raw commands/logs, exact inventories and the staged patch are
sealed below. CRC/member-SHA verification, source hashes and protected-state
checks pass. Primary ad hoc sources/scripts are deleted after sealing. HEAD,
the index and the protected Windows ASan DLL are unchanged. No staging, commit,
push, upload or timing benchmark is performed.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-option-schema-confirmed-v1-20261009.zip` | `30412c4d3b8a14b0a0a1710c20c2a56faccfb27cefd29fef492f64ee7eac6934` |
| `postgres-option-schema-confirmed-v2-20261009.zip` | `de9a6c7c136486fec1259583ed1052f32617ea0367f34d670f11dda1e86785c5` |
| `postgres-option-schema-qualified-v1-20261009.zip` | `317091c407bb61d58d516c8d821da3ff18d3a2745ac43df4941b410e230c393b` |
| `postgres-option-schema-verification-v2-20261009.zip` | `5f80b48e1a289db4acdcf20db31bc1061735849ebf876438651a07ca6d1da00c` |
| `postgres-option-schema-development-v1-20261009.zip` | `b5774430cf61d59c129c851953485bd4ae63e6fa4d29461c249726f665d0fcc3` |

`PQconndefaults` remains Partial: resolved-default enumeration is still absent.
`PQconninfo` remains Open: static descriptors are not live effective-option
snapshots or a round-trippable provider context. The callable counts remain
121 Mapped, 17 Partial, five Open, 42 Different and eight External. Remaining
configuration, transaction/result, deployment and matched-performance work is
not made complete by this metadata gate.

## Configuration Snapshot Prototypes (2026-10-09)

Owning `OptionsInfo` inspection, explicit resolved defaults and retained session
configuration are implemented. These are initial confirmation controls, not the
completed release gate or a full libpq connection-option compatibility claim.
The [configuration contract](postgres-metadata.md#configuration-snapshots)
separates requested settings, historical loader origin and actual negotiated
session metadata.

Confirmation used installed consumers on Windows Debug/Release/Release-ASan and
Linux Debug/Release/Debug-ASan, sequentially with frozen source inputs. Each
profile passed the same eight existing suites: postgres tests, configuration,
system service, credentials, authentication, connection report, failures and
session metadata. Exact nonempty CTest inventories and JUnit checks establish
**48 passing cases**, with no failures, errors or skips.

The new direct controls comprise **66 Weave processes and 36 independent libpq
processes**, including **612 session connections and 918 checked session rows**.
The independently reread raw records verify exact scenario/engine multiplicity,
wire-side connection and query counts, each phase's owning values and retirement
behavior. Windows libpq is pinned to 18.4 and Linux to 18.6. Weave executables
check loaded OpenSSL against headers: Windows 3.6.5 and Linux 3.5.5. Linux ASan
uses `detect_leaks=1:abort_on_error=1`; native controls run separately without
Weave sanitizer instrumentation.

Covered:

- All current nonsecret typed Options fields, nested TLS policy/limits/ciphers,
  host lists/addresses, source policy and OAuth settings, including nondefault
  millisecond values and invalid raw settings inspected without validation.
- Compile-time absence of password, per-host password, SCRAM key, private-key
  passphrase and OAuth-secret fields; boolean provider flags instead of owning
  provider objects. An OAuth closure weak-owner control verifies inspection
  never invokes or extends its provider lifetime.
- Independent copies, database/user fallback, default TLS-policy expansion and
  opaque prebuilt TLS presence. Explicit default loading covers isolated user,
  system-fallback and environment service selection, selected password paths,
  credential cleanup and missing-service errors. Live-storage allocator witnesses
  observe release before free, not reads of freed memory; they are not a guarantee
  about all allocator history or every partial credential fragment.
- Context and blocking sessions, four-worker/16-root affine and work-stealing
  runtimes, and both additional Windows shared-IOCP configurations. Controls
  reject deferred query/reset Tasks, idle pipeline leases, trace/notice/lifecycle
  callback inspection and busy resets without extra peer requests or transport
  retirement. Successful reset changes configuration while old copies remain
  independent; finish retains the last configuration, and moved-from access fails.
- Separate native `PQconndefaults` and `PQconninfo` controls copy only a fixed
  41-key nonsecret whitelist before freeing native descriptors/connections.
  Eight explicitly matched service fields agree. Native default host/database,
  client encoding, application name and SSL policy deliberately need not equal
  Weave defaults; invalid-service fallback also differs. Native fresh-option
  replacement uses finish/reconnect, not a fabricated fresh-options `PQreset`.

Remaining before permanent regression promotion and final snapshot qualification:
failed-reset and admitted transport-failure retention, real file-configured
TLS/mTLS policy retention after credential setup, reduced-module/GSS/LDAP
profiles and relocated packaging/header gates. Provider internals, unsupported
connection keywords and wider deployments are not qualified by these probes.
No timing measurements or performance claims accompany this API work.

The first prototype stopped on a test CMake backslash-path escape error. The
second stopped on an invalid witness assertion: `CredentialPattern::releases`
counts dirty releases, so requiring both a positive count and zero dirty
releases was contradictory. Both failures remain sealed; corrections were
confined to the probes, with production behavior unchanged. The third passed
Windows Debug preliminary controls; the fourth adds the native controls and is
the Windows Debug member of the final six-profile confirmation. No failures
were discarded or selectively retried for a performance result.

Two development-seal checks were also corrected without rerunning any product
gate: the baseline comparator initially included the previous feature's finalized
documentation among code changes, and an XML selector initially included
ProjectReference policy entries rather than only dependency items. Both failed
seal sources and observations are retained in the development archive. Final
checks establish exactly eight production files changed for this feature and
only five documentation/rule files changed after confirmation. Native Windows
projects reference only ZERO_CHECK and libpq dependencies; native Linux Ninja
commands contain libpq but no Weave library or sanitizer flags.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-configuration-info-prototype-v1-20261009.zip` | `b6c00db7be9235943b5e6b47d890fd377c20b94ecdb9657c5457ca776753adc0` |
| `postgres-configuration-info-prototype-v2-20261009.zip` | `28cdf965d277055074976c6732140f16747458041be81453db0b3c80798e2bc1` |
| `postgres-configuration-info-prototype-v3-20261009.zip` | `92cf945da1635036e9f62375552f058ed083579b41c328b2c01160b759313040` |
| `postgres-configuration-info-prototype-v4-20261009.zip` | `a0f7381084298a3454b9a4087933852f761cb5ec6d63b0be6fc971c4e34557bd` |
| `postgres-configuration-info-confirmed-v1-20261009.zip` | `12a95928ec802dee3a77d0922a585ed15a2b42b153eed6467c77ef8b8be6743b` |
| `postgres-configuration-info-verification-v1-20261009.zip` | `d7657915dc097273c652a423fc211bbfcbcbf05ea94af5be3d893384ae56dbf7` |
| `postgres-configuration-info-development-v1-20261009.zip` | `53d44c125efc50e6d5a375b0d9aa0545edc6ecfb12d01237fb7cbb8820d9043c` |

`PQconninfo` advances from Open to Partial, not Mapped: final qualification and
full keyword/provider coverage remain unfinished. Counts are now 121 Mapped,
18 Partial, four Open, 42 Different and eight External, still not a completion
percentage. `PQconndefaults` remains Partial. Nothing was staged, committed,
pushed or uploaded; original staged changes and the protected sanitizer DLL
remain unchanged. Primary ad hoc source files are removed after sealing; frozen
archival copies and generated installed-consumer output remain ignored.

## Configuration Snapshot Regression Qualification (2026-10-09)

The snapshot-specific pending gates above now pass. This qualifies the existing
nonsecret configuration contract, not full connection-keyword compatibility,
provider introspection or the entire PostgreSQL release. No production behavior,
authentication policy or transport code changed during this qualification step.

First, installed-consumer retention probes passed nine sequential, frozen-input
profiles: Windows Debug/Release/Release-ASan; Linux Debug/Release/Debug-ASan;
Windows and Linux Debug without Runtime; and Linux Debug without Runtime, GSSAPI
or LDAP. Independent raw-record verification establishes **684 failure processes
and 14,067 synthetic failure connections**, **90 reset processes and 1,281 reset
connections**, plus **nine real PostgreSQL metadata processes**. These counts
describe that probe matrix only, not an aggregate with later repeated gates.

The three existing failure/report/real-metadata tests were then promoted with
exact source equality to their confirmed retention prototypes. The initial
promotion comparison found one missing blank line, not a semantic mismatch.
New permanent configuration value/session/native controls preserve the earlier
confirmed prototypes, with conditional Runtime compilation, the existing native
version-macro convention and an explicit return in the uninvoked OAuth fixture
coroutine to eliminate MSVC C4033. The owning callback's invocation counter still
proves inspection never calls it. Permanent Python drivers use isolated temporary
sources and the existing owned synthetic-session fixture.

The final permanent matrix repeats all nine profiles with one configure/build/
gate active at a time and unchanged inputs through completion:

- **98 selected regression CTest cases**, with exact nonempty inventories and
  JUnit checks for zero failures, errors or skips. These are selected suites,
  not a claim that every repository test ran. All profiles run postgres tests,
  configuration, system service, credentials, authentication, connection report,
  failures, session metadata, configuration snapshots and option schema. Eight
  profiles also run the opt-in independent libpq configuration control.
- **Three packaging CTest cases**, on Windows Release, Linux Debug and Linux
  minimal Debug. Each builds PostgreSQL in isolation with tests/examples/
  benchmarks off, installs and relocates it, compiles every installed public
  header separately and executes the consumer. New consumer code links raw and
  default snapshot APIs and both connection facades without Runtime or libpq.
- **90 Weave configuration processes and 48 separate libpq processes**, including
  **628 synthetic configuration connections and 942 checked phase rows**.
  The native executable links only libpq plus platform libraries, with no Weave
  archive or sanitizer flags; generated Windows references and Linux Ninja
  commands are independently checked and retained. Native libpq is 18.4 on
  Windows and 18.6 on Linux, with intentionally different built-in defaults and
  missing-service behavior still explicit.
- The same **684 failure processes / 14,067 connections** and **90 selected reset
  processes / 1,281 connections**, now through the permanent suites. Full
  connection-report suites also run their other modes. Failed reset preserves
  original configuration even after transport closure; subsequent valid reset
  installs the new snapshot. Reading configuration never rewrites last_failure.
- **Nine real PostgreSQL metadata processes**, each covering plaintext, opaque
  prebuilt mTLS, file-configured mTLS and encrypted private keys. These exercise
  direct nondefault TLS policy/limits/ciphers, actual client-encoding changes,
  deferred-query/pipeline busy rejection, successful and failed reset, recovery,
  admitted backend termination and finish/move retention. Runtime-enabled
  profiles execute 64 concurrent roots on each four-worker sharded scheduler;
  synthetic suites additionally cover both Windows shared-IOCP schedulers.

The configuration, failure, reset and real-metadata executables check loaded/header
OpenSSL identity: Windows 3.6.5 and Linux 3.5.5. Linux ASan retains leak detection.
Certificate/key fixture directories are
independently checked absent after each real process; the owned encrypted-key
file is removed before its enclosing certificate fixture. The server driver
uses fresh owned primary/standby deployments, never existing/system datastores.
Third-party providers and server binaries are not all sanitizer-instrumented.

The first retention driver failed before launching its Windows client because
it passed a Windows-format executable path directly to WSL subprocess creation. That
failed attempt is retained; only the driver path conversion changed. The first
permanent Windows Debug pass retained the uninvoked fixture's compiler warning;
the final full matrix removes that warning. No failure was silently discarded,
and no timing measurements or performance claims accompany this API work.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-configuration-retention-prototype-v1-20261009.zip` (failed driver) | `7268e1ee887ca3e56ff3734e624e7b9b3d5ba08ab4089eee21928098bd95a552` |
| `postgres-configuration-retention-prototype-v2-20261009.zip` | `27a2517a5829c57511cbc821936d1570f27e4871b4cce4fa958d79d5e2525670` |
| `postgres-configuration-retention-confirmed-v1-20261009.zip` | `13023c265ab1fff10cd2b810bf293667556b3f36b7555ad8676bb202a6ae542f` |
| `postgres-configuration-retention-verification-v1-20261009.zip` | `1a029ab981e64c66bbee4c7e7df15244a3b99342a956c96e8938a348710c8747` |
| `postgres-configuration-permanent-v1-20261009.zip` (preliminary Debug) | `32f27a93c14e0f655e710ee07953221444b2de104a7ebfb942c9ec643d6cff15` |
| `postgres-configuration-permanent-v2-20261009.zip` | `e0eb7900032dde9565c12c15b23454b1f0571102c70f06cc44d55e4050bebb74` |
| `postgres-configuration-permanent-verification-v1-20261009.zip` | `77e2d41f16e852e5f549c54741dc17449337f002692eaa624d4ddef01bbe8eb9` |

Development source/diff boundaries, failed-driver evidence, promotion differences
and final documentation are retained in
`postgres-configuration-retention-development-v1-20261009.zip`; primary ad hoc
source/driver files are removed after sealing. The original index, HEAD and
protected sanitizer DLL remain unchanged; nothing is staged, committed, pushed
or uploaded by this step.

`PQconninfo` and `PQconndefaults` remain Partial because the documented owning
nonsecret snapshots do not enable every connection keyword or export opaque
providers. Inventory counts remain **121 Mapped, 18 Partial, four Open,
42 Different and eight External**, not a completion percentage. Transaction
progress/unknown states, result-memory/selective-copy/application-result hooks,
standalone chunks, queueable Flush, remaining connection options, deployment
soaks/security review and final matched measurements remain on the full goal.

## Transaction Progress And Unknown State (2026-10-09)

The transaction gap above is now closed for the documented contract. Both
connection facades expose idle, open-transaction (`active`), failed,
`in_progress` and `unknown` states synchronously. Existing enum values are
preserved; admitted `info().transaction` uses the same classification.
[Semantics and the intentional pipeline difference](postgres-metadata.md#transaction-state).
No transport, cancellation, coroutine-lifetime or scheduling policy was redesigned.

Installed-consumer prototypes first passed six Windows/Linux Debug/Release/ASan
profiles, with **60 selected CTest cases, 120 Weave processes, 12 independent
libpq processes and 1,518 synthetic transaction connections**. Further installed
probes confirmed failure-state inspection and real-server row/COPY assertions
on Windows Debug and Linux Debug-ASan before permanent test promotion.

The final permanent matrix covers nine profiles: Windows Debug/Release/Release-ASan;
Linux Debug/Release/Debug-ASan; Windows/Linux Debug without Runtime; and Linux
Debug without Runtime, GSSAPI, LDAP or optional native controls. Independent
verification of the raw records establishes:

- **35 selected regression CTest cases**, with exact nonempty inventory/JUnit
  agreement and zero failures, errors or skips. Each profile runs the new
  transaction suite, existing PostgreSQL suite and extended failure suite.
  Eight also run the separate native libpq transaction control.
- **Three package CTest cases**, on Windows Release, Linux Debug and Linux
  minimal Debug: isolated library-only builds, relocated installs, standalone
  public headers and consumer compilation/linking of both scalar getters and
  all enum values. Package consumers require neither Runtime nor libpq.
- **144 Weave transaction processes, 16 native processes and 1,554 synthetic
  transaction connections**. Coverage includes deferred queries/reset, notices,
  passive notification waits, row/COPY/exchange intervals, queued and unsynchronized
  pipelines, receive-first split sending and later Sync debt, finish/reset/move,
  malformed ReadyForQuery, EOF and explicit terminal cancellation. Runtime
  variants use four workers and 16 concurrent independent sessions on both
  schedulers, including both Windows shared-IOCP modes.
- **684 failure-suite processes and 14,067 synthetic failure connections**.
  Existing admitted-operation cancellation, malformed/resource/transport failures
  and normal SQL recovery now check known versus unknown transaction state,
  including four-worker 32-root variants. Counts include nine formatting-only
  processes, which do not open a connection.
- **Nine owned real PostgreSQL metadata processes**, covering plaintext, opaque
  mTLS, file-configured mTLS and encrypted keys. Actual BEGIN/error/ROLLBACK,
  row/COPY streaming, serialized trace callbacks, reset/recovery, backend termination
  and moved/finished sessions are checked. Runtime profiles run 64 concurrent
  roots on each four-worker sharded scheduler. All nine owned certificate/key
  directories are independently verified deleted; existing/system datastores
  are not touched.

OpenSSL header/runtime identity is checked: Windows 3.6.5, Linux 3.5.5.
Linux ASan retains leak detection. The independent libpq controls are 18.4 on
Windows and 18.6 on Linux; generated MSBuild references and Linux Ninja commands
prove they link no Weave library and inherit no Weave sanitizer instrumentation.
Third-party dependencies/server binaries are not all sanitizer-instrumented.
These are selected correctness gates, not a full repository run or performance
measurement. The primary suite is ordinary correctness CI; native controls
remain opt-in, and no timing benchmarks were added or run by this change.

Retained failed attempts are attributable. Prototype v1 requested a nonexistent
standalone pipeline-streaming target; that test already belongs to the main
PostgreSQL suite. Prototype v2 incorrectly expected COPY OUT EOF to remain in
progress even though it drains ReadyForQuery. Prototypes v3/v4 incorrectly
expected native libpq to stay active after consuming an unsynchronized command;
both native versions actually returned idle. The final implementation documents
Weave's conservative synchronization boundary instead of hiding that difference.

The permanent driver completed seven profiles before an incorrect expected LDAP
flag rejected the Linux no-Runtime cache. Its correction completed that profile,
then found optional libpq controls enabled in the minimal cache. The final minimal
run explicitly disables those controls. No production/test source changed between
these three fragments; raw verification combines their completed profiles while
retaining both interrupted runs as failures, not passing whole-matrix runs.
Two initial verifier attempts also remain retained: scenario-major record order
and MSBuild default properties versus actual ItemGroup references were corrected.
No test failure or measurement was selectively discarded.

| Sealed Ignored Archive | SHA256 |
| --- | --- |
| `postgres-transaction-prototype-v1-20261009.zip` (failed driver) | `89bca469600bc5222a7494a6c8d4a6923b2f3a2cdcd3136a83cc321329ae7d45` |
| `postgres-transaction-prototype-v2-20261009.zip` (failed expectation) | `2160477d2d9422995eee29b2e5783cd99c66001f618f0b8686eb78b50d0320df` |
| `postgres-transaction-prototype-v3-20261009.zip` (failed native expectation) | `e66c7db84c7528205c83b36d66b8f63835e5c8969c932cc19c1bee8c1176638b` |
| `postgres-transaction-prototype-v4-20261009.zip` (native diagnostic) | `77be4f4f96c3bc397f285a90c7861a9698787201fc2781add1dd543b8ea7272b` |
| `postgres-transaction-prototype-v5-20261009.zip` | `a4a45cbb30cae59df935757fa5374926fb074ed96a06b2f940d54b4410644e8f` |
| `postgres-transaction-confirmed-v1-20261009.zip` | `dc5a492d8cd2651a3a04ab3a33405c4010ef2f8a1ec0407b1ec7a74e78705774` |
| `postgres-transaction-verification-v1-20261009.zip` | `9de3b1429b78b058028d5438241bf9d521f20bfab3b154dca18cea0aaec36539` |
| `postgres-transaction-extended-v1-20261009.zip` | `68150c3788764d0c8631def01e37b79fe800ceaacc68f09650dccc935b0d94f1` |
| `postgres-transaction-permanent-v1-20261009.zip` (seven profiles; failed cache guard) | `8b49381354aa3d5298b6fad2d3f9759d62fe2b5c167448a3d2535efb6f9b2629` |
| `postgres-transaction-permanent-v2-20261009.zip` (one profile; failed cache guard) | `37a06d7b99c6d56f4e8f34889100e6e2f3650e82776a8ceab8ab1b81f993081c` |
| `postgres-transaction-permanent-v3-20261009.zip` | `692ef9794dff57649710f62d0a7f3376b89cb6ae0193be4b79337201ff4a531a` |
| `postgres-transaction-permanent-verification-v3-20261009.zip` | `0d991747d066efa7bb0ffd0ee27143145dd5bd913bb6a4da60f72aa30f41fd94` |

Source/diff boundaries and failed verifier records are sealed in
`postgres-transaction-development-v1-20261009.zip`; primary ad hoc sources are
removed afterwards. The original index, HEAD and protected sanitizer DLL remain
unchanged. Nothing is staged, committed, pushed or uploaded by this step.

`PQtransactionStatus` advances from Partial to Mapped, with the pipeline lifecycle
difference explicit. The inventory is now **122 Mapped, 17 Partial, four Open,
42 Different and eight External**, not a completion percentage. Pipeline-status
inspection, result-memory/selective-copy/application-result hooks, standalone
chunks, queueable Flush, remaining connection options, deployment/security review
and final matched measurements remain unfinished on the full goal.

## Connection Pipeline Status (2026-10-09)

`Connection::pipeline_status()` and `BlockingConnection::pipeline_status()` now
expose synchronous `noexcept` off/on/aborted inspection. They read the existing
owning lease and mutex-protected abort-until-Sync latch; no coroutine, transport,
framing, cancellation, result correlation or admission policy is redesigned.
The [contract](postgres-metadata.md#pipeline-state) distinguishes mode/recovery,
transport health and SQL transaction state. Owning ConnectionInfo deliberately
does not duplicate a field that its admission policy would always report as off.

Before permanent tests, six frozen Windows/Linux Debug, Release and ASan
profiles pass 150 Weave process cases, 18 independent native libpq cases and
30 selected regression CTest entries. The synthetic fixtures observe **1,818
connections**, exact command/Flush/Sync/closure counts and positive assertion
counts. Runtime cases use four workers and 16 independent connection roots,
both schedulers and, on Windows, both sharded and shared IOCP layouts.
Separate installed-consumer Windows Debug/Linux ASan real-server probes pass
plaintext, opaque mTLS, file mTLS and encrypted-key profiles, including four-worker
64-root workloads with both schedulers. These are correctness, not timing runs.

Four earlier failed prototype controls are retained rather than counted as
passes. The fixture initially closed before reading the expected Flush; it now
waits for that request. A cancellation observer was initially registered after
acquiring the pipeline lease and correctly rejected as busy; the probe installs
it before the lease and retains its CancelSource through cleanup. Two native
assumptions were also wrong: libpq permits consumed-unsynchronized exit, and an
unknown framed backend tag yields a fatal result without marking its connection
bad. The Weave production implementation is byte-identical through all prototype
attempts and the final permanent gate; no library behavior changes accommodate
these fixture/native discoveries.

The final frozen permanent gate passes **52 regression CTest entries and three
package CTest entries** across nine profiles: Windows/Linux Debug, Release and
ASan, Windows/Linux without Runtime, and Linux without Runtime/GSSAPI/LDAP/native
comparison controls. Native controls are explicitly enabled in the other eight
profiles; their generated MSBuild/Ninja inputs contain no Weave library link or
sanitizer instrumentation. They load libpq **18.4 on Windows and 18.6 on Linux**;
Weave ASan checking remains enabled in its own profiles.

Independent verification checks raw verbose CTest output rather than truncated
JUnit output. Exact nonempty inventories and JUnit case names agree, every case
runs with no failure/error/skip, and all command exit codes and frozen source
hashes agree. The permanent pipeline controls pass **180 Weave and 24 native
process cases**, observing **1,860 synthetic connections**. The surrounding
unchanged transaction/failure selections pass another 844 peer process cases.
Coverage includes empty/moved leases, deferred/unsent work, SQL errors and skipped
commands, queued versus acknowledged Sync, buffered unconsumed results, failed
SQL transactions, receive-first split traffic, reset rejection, callbacks, EOF,
malformed traffic, observed-pending cancellation and sent-unsynchronized drop.

Nine disposable PostgreSQL server runs pass the promoted real controls. Each
tests all four transport/credential profiles; Runtime-enabled profiles also run
64 simultaneous sessions per four-worker scheduler. Existing row/COPY, metadata,
reset/failure and lifetime controls remain active. All nine certificate fixtures
are confirmed removed. Package gates cover Windows Release, Linux Debug and
Linux minimal isolated/relocated consumers and standalone public headers.

Native controls make two intentional lifecycle differences explicit. Weave
requires acknowledged Sync before finish, whereas the tested native exit allows
consumed-unsynchronized commands. Weave clears the server abort latch when a
valid Sync is parsed, not when the application consumes the corresponding result.
Both mode inspection APIs can remain on after transport failure; Weave's stricter
malformed-message retirement is not a claim of identical native connection health.
These controls confirm the useful [libpq pipeline-state
capability](https://www.postgresql.org/docs/18/libpq-pipeline-mode.html), not a
literal polling/ownership model or general shared-session thread safety.

Ignored sealed evidence:

| Archive | SHA-256 |
| --- | --- |
| postgres-pipeline-status-prototype-v1-20261009.zip | c43c96fafbf71d41a95d54cca76063b480742e980d807143bad90096296d8d2d |
| postgres-pipeline-status-prototype-v2-20261009.zip | 3208c42e2923021ccb2b1b40558c1252d49fa1d79917b7c1b22de0cd5108265f |
| postgres-pipeline-status-prototype-v3-20261009.zip | c94e83d55bb112de78dd131fa2f0e02f9da3fc4e6b4ad42af8c58ff0c49f9c8d |
| postgres-pipeline-status-prototype-v4-20261009.zip | 5de59ec507ab7b375e44ba4ab6b50013c26fc8c27e1676f493e2b10ec0cd7aa6 |
| postgres-pipeline-status-prototype-v5-20261009.zip | a02b3527853b588d5afff6ee55c40dd9554b0151322713f7bc7253985904a1fa |
| postgres-pipeline-status-confirmed-v1-20261009.zip | b3e32bd38be48ce67277d2bc5df23a13e733cad399a8a56d9a8e75faf56f37c7 |
| postgres-pipeline-status-real-prototype-v1-20261009.zip | 7dafed46783fb127e98a258b3c663db75b2c4976f8acbd4e4c75bcf7ca0f8853 |
| postgres-pipeline-status-permanent-v1-20261009.zip | 3d0f24b2a537606f9834e1f975e584ed07f6d0af2e2a7d3cacbfc17e80e85aa3 |

The development archive retains the exact before/after changes, source/manifests,
probe/verifier scripts and independent verification records. Primary ad hoc
sources are removed after sealing; index, HEAD and the protected sanitizer DLL
remain unchanged. This feature stages, commits, pushes and uploads nothing.

`PQpipelineStatus` advances from Partial to Mapped. The callable inventory is now
**123 Mapped, 16 Partial, four Open, 42 Different and eight External**, not a
completion percentage. Result-memory/selective-copy/application-result hooks,
standalone chunks, queueable Flush, remaining connection options, deployment
soaks, independent security review and final matched measurements remain on
the full goal. These affected-feature gates do not establish full libpq parity
or universal production/security readiness.

## Selective Result Copy Qualification (2026-10-09)

`ResultSet::copy(ResultCopyOptions)` now selects columns, rows and lifecycle
observers synchronously. Rows imply columns; kind, command, parameter types and
suspended state are always retained. Selected payload is populated before hooks.
Observer-free copies retain no callback/data owners, and the source's active
lifecycle-dispatch contract still applies. Default special members, wire parsing,
Task semantics and transport behavior are unchanged; [public contracts](postgres-results.md#selective-copies).

The two production files stayed byte-identical from the initial prototype through
all confirmation and permanent gates. Prototype confirmation covers all eight
selection combinations, every Weave result kind, binary/NUL/empty/NULL values,
large rows, deep independence, cloned/shared observer data, observer veto, retained
results after Connection destruction, eight-thread immutable-source copies and
four-worker/32-session roots on both schedulers. Windows also tests shared IOCP.

The final permanent matrix passes **56 CTest entries**: 53 affected regression/
native entries across Windows/Linux Debug, Release and ASan plus Runtime-disabled
Windows/Linux and Linux GSS/LDAP-disabled builds; three additional component
packaging gates verify standalone headers and relocated installed consumers that
actually call the new API. Extended lifecycle tests preserve ordinary concurrent
copy coverage as well as selective copies. Their exact process inventory is
45 successful Weave executions and nine explicitly expected reentrancy deaths,
with 2,376 owned synthetic connections. The death records identify the new source
guard, not an unrelated sanitizer or teardown failure. Notification-fixture changes
only add connection-count diagnostics; its existing suite is included.

Nine disposable PostgreSQL 18 runs pass Context/blocking plain and verified mTLS
controls for query, prepare/describe, empty results, pipeline chunks and COPY.
The six Runtime-enabled profiles additionally pass four-worker/16-root workloads
on both schedulers. Reduced builds report no Runtime work, not invented worker
counts. All nine owned certificate fixtures are confirmed removed.

Eight separate native executables link only libpq, never Weave or ASan, and assert
the actual Windows 18.4/Linux 18.6 library version. They cover six native statuses
and all eight attribute/tuple/event selections, binary/empty/NULL payloads, schema
metadata and rejected-copy callback lifetime. They use inert failed connections
with no native socket, not an existing database service. Native copies always
become TUPLES_OK and discard error text; Weave deliberately preserves its kind
and uses separate Outcome diagnostics. Connection-owned notice handlers are not
result notice hooks, and those native flags are not claimed as tested equivalents.

Two early failed probes remain archived: the first driver wrongly rejected the
test's success message on stderr; the second Windows Debug death probe timed out
on CRT abort reporting. Test-only output and abort handling were corrected before
confirmation. Neither failure caused a production implementation change.

Ignored frozen evidence and SHA-256:

| Archive | SHA-256 |
| --- | --- |
| postgres-result-copy-prototype-v4-20261009.zip | 913db3dd313e81f50ee767b221eeb000fadf2e7a1ecb4ca8b50917ca06bac0bd |
| postgres-result-copy-confirmed-v1-20261009.zip | ddb0a6503cfa400fec4204fbd6b2016c277ff4cf03c2d4c96f87e7d7939366b1 |
| postgres-result-copy-permanent-v1-20261009.zip | 7a9cdb2c7fe1fe9885d937de8f1404d97673ff05bea1520e12090591dca4d856 |

Independent verification checks ZIP members/hashes, exact inventories/JUnit,
raw child outcomes, contract locations, native linkage, fixture cleanup and
unchanged production/protected state. The development archive retains the
before/after changes and primary probe/verifier sources; those exploratory files
are removed after sealing. No stage, commit, push, upload or timing run occurs.

`PQcopyResult` advances from Partial to Mapped with the documented model
differences. The inventory is **124 Mapped, 15 Partial, four Open, 42 Different
and eight External**, not a completion percentage. Result-memory reporting,
application-built observer attachment, standalone chunks, queueable Flush,
remaining connection options, deployment soaks, independent security review and
final matched measurements remain on the full goal. These gates do not establish
complete libpq parity or universal production/security readiness.

## Application Result Attachment Development (2026-10-09)

The development API `Connection::attach_events(ResultSet &)` and its synchronous
BlockingConnection forwarder attach owning lifecycle registrations after the
application populates a result. Existing accepted EventIds are skipped; rejected
entries release their data and can be retried explicitly. Every eligible observer
is attempted and the first failed Result is returned without undoing accepted
entries. Calls can explicitly merge live registries without replacing existing
state; [contracts and limits](postgres-events.md#application-built-results).

Six frozen Windows/Linux Debug, Release and ASan installed-consumer probes pass
30 Weave processes, six independent libpq processes and 30 affected regression
CTest entries. Exact owned synthetic connection count is 1,758. Four-worker
controls use 32 independent session roots on both schedulers; Windows additionally
uses shared IOCP. Retained application results support eight-thread immutable
copies after both source Connections die, with registration/data-owner drain
witnesses. Controls cover repeated/partial attachment, retry, cloned state, all
seven Weave result kinds, binary/empty/NULL payload, reset/move/closed Connection,
deferred Tasks, pipeline leases, blocking forwarding and cross-Connection callback
reentry rejection before receiver locking.

A confirmed initial implementation error aggregated numeric error codes rather
than failed Results. A targeted probe reproduced overwriting the first failure
when it contained a zero-valued error code. The helper now stores Result<void>
itself; controls cover both zero-first/multiple failures and a lone zero-code
failure. The failed v2 archive is retained. Production stayed byte-identical from
the corrected two-profile v3 confirmation through expanded six-profile v4.

Independent libpq executables never link Weave or ASan. They assert the loaded
Windows 18.4/Linux 18.6 version, construct results with inert failed connections
without native sockets, and cover six native statuses, pre-populated binary
payload, failure/partial success, retries only for uninitialized hooks and exactly
one destroy hook per accepted registration. This qualifies those callback
behaviors, not literal equivalence of observer snapshot timing or native error
result representation. The callable audit remains unchanged pending promotion.

An earlier selective-copy guide incorrectly described the callback destination
payload as mutable. Event::result is a const pointer; source inspection and
installed-consumer static assertions confirm the read-only contract. Only the
instance-data slot is mutable. The guide is corrected without changing that API.

Ignored evidence and SHA-256:

| Archive | SHA-256 |
| --- | --- |
| postgres-result-attach-prototype-v1-20261009.zip | 91b1b972e9b287cb100b1eed4508d06138ab3d39837cc29189d547b4074de717 |
| postgres-result-attach-prototype-v2-20261009.zip (failed first-error probe) | 2a89e43c5ded86823b67e5d25519a4ff610065da69ae0ab5f9589bb9790d940b |
| postgres-result-attach-prototype-v3-20261009.zip | 510dc6d63e49f04ac37596a6d5a91f23e0a399ccc3f7cee922f69aa220a3031f |
| postgres-result-attach-prototype-v4-20261009.zip | 0320cf9ca3ba7a856fb7da34278b7737d6aa2092824e8e07c9f08d763e0e80d1 |

Verification independently checks member hashes, frozen inputs, exact inventories/
JUnit, raw process outcomes/counts, native linkage and the precise failed-probe
assertion. This is development confirmation, not completed feature promotion:
permanent tests, real-server/mTLS attachment controls, reduced-module builds and
component packaging are still pending. Active ignored exploratory sources remain
available for that work and must be archived/removed before commit. No stage,
commit, push, upload, timing run or full-release readiness claim occurs.

## Application Result Attachment Qualification (2026-10-09)

The confirmed attachment implementation now has permanent tests. The six
production files are byte-identical to the corrected development implementation;
automatic wire-result creation/copy/destruction and Task error propagation are
unchanged. `Connection::attach_events` and `BlockingConnection::attach_events`
are synchronous Result-returning operations, not awaitable transport work.

Before promotion, expanded installed-consumer prototypes passed Windows Debug
and Linux ASan against disposable PostgreSQL 18 over plain TCP and verified mTLS.
They exercise accepted registrations whose instance data was subsequently cleared,
ensuring empty data does not cause repeated creation. The final test source is
byte-identical to that prototype; the independent native source only changes its
version-macro name. Permanent registration makes Runtime conditional, with separate
opt-in libpq and real-server targets. No comparison dependency enters the library.

The frozen permanent matrix passes:

- Windows Debug, Release and ASan, including both four-worker schedulers and
  sharded/shared IOCP attachment controls.
- Linux Debug, Release and ASan, including both four-worker schedulers with io_uring.
- Windows/Linux PostgreSQL-only builds without Runtime or LDAP, plus Linux with
  Runtime, GSSAPI, LDAP and native libpq controls disabled.
- Exactly 56 CTest executions: 45 affected Weave suites, eight independent native
  attachment controls and three component-packaging gates. Inventories and JUnit
  agree; there are no failed, errored or skipped cases, and empty selections fail.
- 36 successful attachment probe processes and 1,773 observed synthetic peer
  connections, with exact mode inventories, empty stderr and no fixture errors.
- Nine additional disposable real-server runs, each covering Context and blocking
  operation over plain TCP and verified mTLS. The six Runtime-enabled profiles
  also run 32 independent roots per scheduler on four workers, alternating transport
  profiles. Every root owns its session; no shared mutable Connection is implied.
- Concurrent immutable-source copies, per-registration callback serialization,
  binary/empty/NULL payload retention, every ResultKind, move/reset/logical-close
  handling, retained registrations after Connection destruction, deferred Tasks,
  pipeline leases, same/cross-Connection nested dispatch, partial acceptance and
  first-error retention, including failed Results carrying a zero error code.
- Matching libpq 18.4 on Windows and 18.6 on Linux. Native controls independently
  populate six PGresult statuses and verify partial creation, retry, accepted-hook
  skipping and exactly-once destruction. They link no Weave library and have no
  sanitizer instrumentation; Weave ASan/leak checks remain enabled separately.

Packaging covers isolated library-only builds, relocated imports and standalone
public-header probes on Windows Release, Linux Debug and Linux minimal. Volatile
member-pointer references force both attachment symbols to link in optimized,
network-free package consumers; those package mains do not execute attachment.
Actual installed-consumer attachment execution is covered by the earlier
six-profile synthetic probes and the two expanded real-server prototype profiles,
not claimed for every relocated package profile.

The feature deliberately selects the supplied Connection's current registrations
after payload population. It does not retain libpq's pre-population pending
registry, manufacture arbitrary native error-status results, validate SQL/schema,
add thread safety or make application allocations recoverable. Result callback
payloads remain read-only. Accepted hooks retain their own data policy; rejected
instances have no later destroy hook. These distinctions leave PQmakeEmptyPGresult
Partial while the explicit result-create operation is now mapped.

Ignored evidence and SHA-256:

| Archive | SHA-256 |
| --- | --- |
| postgres-result-attach-real-prototype-v1-20261009.zip | c5fdc6eda5e53a6d52dfd9afa46d3d7bec10352214e9a902cad37fd2db501547 |
| postgres-result-attach-permanent-v1-20261009.zip | e7d6efc51838f2e04f58e69bf90798f70edf68bed30d520b07b8ee1a0820b9aa |

Independent verification checks archive CRC/member hashes, complete frozen source
inputs, promoted-source identity, unchanged production, exact process/test/native
inventories, all real-server profiles, certificate cleanup and actual native
project/link commands. One verifier assertion initially expected `-lpq`; Ninja
uses the resolved `/usr/lib/x86_64-linux-gnu/libpq.so` path. The assertion was
corrected against the actual link command without rerunning or discarding gate
results. This was a verifier assumption, not a library/test failure.

HEAD, the staged patch and the protected Windows ASan runtime remain unchanged.
No benchmark timing, stage, commit, push or upload is performed. Result allocation
reporting, standalone chunks, queueable Flush, remaining configuration capabilities,
deployment soaks, independent security review and final matched measurements remain
open. These scoped gates do not establish full libpq parity or production readiness.

## Queueable Flush Development (2026-10-09)

The development API adds synchronous `Pipeline::request_flush()` and
`BlockingPipeline::request_flush()`. They append a bounded five-byte backend
Flush at the current request-buffer position without transport I/O, implicit
Sync, sequence-ID consumption, command admission or a result event. Existing
send/receive/duplex snapshots retain their automatic trailing Flush.

Byte-only snapshots now submit even when no response-producing entry exists.
Entry collections are checked before updating submitted sequence/barrier debt.
Flush alone does not make an idle session unsynchronized or clear an abort latch.
Finish rejects unsent markers and reserved outbound bytes; wholly unsent pipeline
destruction still discards its local queue. Other command/result/cancellation
semantics are unchanged. Four production files differ from the attachment checkpoint.

Installed-consumer prototypes pass Windows/Linux Debug, Release and ASan, including
both four-worker schedulers with 32 independent roots. Windows also covers sharded
and shared IOCP; Linux uses io_uring. The independent peer buffers responses until
Flush/Sync, records every frontend frame in order and checks exact per-session
wire sequences rather than accepting only successful client exit codes.

Controls cover empty/flush-only queues, idle transaction state, deferred Tasks,
pipeline moves, logical finish/closed errors, dropping wholly unsent markers,
receive-first reservation, split sends, exact placement between SQL/Sync frames,
correlation-ID preservation, SQL abort/recovery, repeated flush-only sends,
byte-limit rejection and accepting markers despite full command admission.
The blocking facade also checks sent-window rejection.

The six-profile frozen run passes 30 successful Weave prototype processes,
six independent libpq controls, 1,176 observed synthetic connections and exactly
30 affected CTest suites. Inventories and JUnit agree, with no failed, errored or
skipped tests and empty selections rejected. Native libpq is 18.4 on Windows and
18.6 on Linux; actual project references/link commands confirm no Weave linkage
or native sanitizer instrumentation. Weave leak/ASan checks remain enabled.
Production source bytes are identical across all three prototype archives.

| Ignored Archive | SHA-256 |
| --- | --- |
| postgres-flush-request-prototype-v1-20261009.zip | c5ad76611655b329acd804f4057799a63bcedefce51f80bdf4719aa007704a39 |
| postgres-flush-request-prototype-v2-20261009.zip | 1b8d78ab0390a3c81dced590f5336ef896505b933cb557d62cbf66671665dd88 |
| postgres-flush-request-prototype-v3-20261009.zip | a08599ad2950f24d9c83763ba8387e14f20e8072c0618dbcb6ab6da66ba8d431 |

Independent verification checks archive CRC/member hashes, frozen source inputs,
exact mode/frame/process inventories, native dependency boundaries, inventory/JUnit
agreement and unchanged HEAD/staged patch/protected ASan runtime. Six-profile
prototype success is not permanent regression promotion or release qualification.
At this development checkpoint, real PostgreSQL/mTLS, cancellation-specific
expansion, reduced-module and relocated component packaging remained pending.
The later permanent qualification below closes those feature-specific gates;
the checkpoint retains its original exploratory sources for evidence.

Allocation-reporting investigation separately confirms a public-storage design
constraint: ResultSet exposes mutable default-allocator standard strings/vectors.
The qualified MSVC STL adds iterator-proxy allocations in Debug and alignment
overhead for large buffers; small-string storage also varies by implementation.
Exact allocation requests cannot be inferred by a portable capacity sum. No
estimate or STL-private inspection API is introduced. Exact owned-allocation
accounting remains Open and needs an explicit storage/ownership design; this does
not remove it from the parity goal or replace it with wire-data limits.

## Queueable Flush Regression Qualification (2026-10-09)

The locked implementation now has permanent `flush_request.cpp` controls,
an independent protocol fixture in `flush_request.py`, and an opt-in, separately
linked `flush_request_libpq.cpp` baseline. The same Weave source builds the
real-server control with a private test-only definition. Library consumers do
not acquire Python, libpq, server headers, Runtime or fixture dependencies.

Before promotion, an expanded installed-consumer prototype passes Windows Debug
and Linux ASan: 46 successful Weave processes, two native libpq controls,
788 synthetic connections, ten affected CTest entries and two real-server
processes. Cancellation before submission preserves a wholly unsent marker and
reusable session. Cancellation after a server notice closes the advanced session,
drains the task scope and preserves the primary cancellation error. Orderly EOF
and a well-framed unknown backend message preserve their terminal errors through
later request_flush/finish attempts. Every synthetic connection has an exact
frontend-frame sequence assertion, including the empty-result paths.

Three failed expanded probes are retained, with no production changes:

- v4 registered a notice callback after acquiring a pipeline lease, which correctly
  returned busy. Registration now occurs before lease acquisition.
- v5 attempted an EOF fixture while the client's automatic trailing Flush remained
  unread. v6 records native Winsock error 10054 in the system category, identifying
  a TCP reset rather than the intended orderly EOF. The corrected fixture drains
  both Flush frames before shutdown(SHUT_WR); it does not relax the expected error.

The permanent frozen gate passes Windows/Linux Debug, Release and ASan,
Windows/Linux without Runtime or LDAP, and Linux without Runtime, LDAP, GSS or
libpq controls. Linux uses io_uring. Runtime synthetic controls use four workers,
both schedulers, 32 roots for ordinary sessions and 16 for terminal cases;
Windows also covers shared and sharded IOCP. Roots own separate sessions.

All nine profiles pass real PostgreSQL 18 controls over plain TCP and verified
mTLS/SCRAM channel binding. Runtime real controls use four workers and 16 roots
per scheduler, alternating transport policies. Each deployment is freshly owned,
not a pre-existing service; its certificate fixture is confirmed deleted.
The blocking facade is exercised on the calling-thread Context as well.

The gate passes exactly **65 CTest entries**, including three isolated/relocated
PostgreSQL packaging controls, **162 successful Weave synthetic processes**,
**eight independent libpq controls**, **2,393 synthetic connections**, and
**nine real-server processes**. CTest inventories and JUnit agree with no failures,
errors, skips or empty selections. Installed consumers retain references to both
new synchronous public methods without attempting network I/O; actual installed
API execution was checked by the earlier prototypes. Native project references
and Ninja link commands confirm libpq-only linkage and no sanitizer instrumentation;
Weave's sanitizer/leak checks remain enabled.

| Ignored Archive | SHA-256 |
| --- | --- |
| postgres-flush-request-prototype-v4-20261009.zip (failed fixture) | ee1d0a11efafe9b94a7db197836503ced2934b1da7d00b23e76086499453965d |
| postgres-flush-request-prototype-v5-20261009.zip (failed fixture) | 2cd23aa2b7e85527410800781de775e6597e004c5804fb9cd8904076d4347a3f |
| postgres-flush-request-prototype-v6-20261009.zip (reset diagnostic) | a3e48592394e0b89e13498866441f7e475112e79d58ce4e857c4dc951021ee92 |
| postgres-flush-request-prototype-v7-20261009.zip | 7052247038742f7a5c36e430d5150f26bad5c065306aad8fd1e057db2454851e |
| postgres-flush-request-permanent-v1-20261009.zip | 81153ad7292d75810c6f6d30d37eee59c7b8ed89af57f25086710b55a8e0d125 |

Independent verifiers check archive CRC/member hashes, frozen sources, exact
process/mode/frame inventories, positive controls, native dependencies,
inventory/JUnit agreement, fixture cleanup and unchanged HEAD/staged patch/protected
ASan runtime. Production bytes are identical across all seven prototypes and
permanent promotion; promoted C++ tests match the confirmed prototype apart from
the native version-definition name. The Python fixture only gains native-only
CLI selection and a version-independent positive marker; frame/error expectations
are unchanged.

This maps the independently queueable Flush capability, not native polling or
partial-write APIs. Weave intentionally keeps its automatic trailing Flush,
bounded queue and explicit transport-driving semantics. No timing benchmark,
independent security audit or whole-module release certification is implied.
Exploratory primary sources are archived and removed before commit; evidence
remains ignored. No staging, commit, push or upload is performed here.

## Standalone Exchange Chunks Development (2026-10-09)

The development API adds RowOptions to Connection/BlockingConnection::exchange.
Default calls keep their buffered behavior. Positive chunk_rows yields owning
row_chunk results before command completion, followed by a zero-row tuples
result carrying the actual command tag. Every returned chunk copies its schema.
The existing pending-message slot preserves partial tails and rows deferred by
retained-data pressure; no new driver, background reader or coroutine wrapper is
introduced. SQL errors discard undelivered rows and retain the existing
ReadyForQuery recovery boundary. COPY phases and deferred-task leases are unchanged.

Logical retained charges reserve both staging and delivery schema. Row count is
an upper bound: streaming a hundred rows through a 2 KiB retained bound emits
smaller chunks rather than retaining the entire result. A single row that cannot
fit with its schemas returns resource_limit and closes the advanced session.
Decoder/input scratch and caller-retained results are not an exact heap/RSS budget.

Two failed initial probes are retained. v1's fixture asserted only protocol 3.0,
while the configured Weave default requests 3.2. The fixture now accepts both
supported protocol versions. v2 passed the complete ordinary session sequence,
then correctly rejected a test configuration with message_bytes=4096 and
result_bytes=2048 as invalid_argument. The corrected fixture uses equal 2 KiB
bounds; its oversized-row case stays inside the wire-message limit but exceeds
row-plus-schema retention. No production source changed between these probes.

The corrected initial prototype passes Windows Debug and Linux ASan. Expanded
controls then pass all six Windows/Linux Debug, Release and ASan profiles using
installed consumers, native libpq controls and actual PostgreSQL 18 over plain
TCP and verified mTLS/SCRAM channel binding. Runtime uses four workers and both
schedulers with 16 independent roots; Windows also covers shared/sharded IOCP.
Linux uses io_uring. Roots own independent sessions and observers.

Controls cover buffered and 1/2/3/5/7-row modes, full/partial/zero-row completion,
empty queries, multi-statements, mixed COPY OUT/query events, NULL versus empty
values, owning metadata, callback creation/destruction, deferred next/move/finish
guards, SQL errors after delivered rows, adaptive retention and oversized rows.
Cancellation follows an observed server notice; EOF and malformed DataRow
responses preserve terminal behavior after an already-delivered owning chunk.

For early-progress controls, the peer sends only the schema and first two rows.
It withholds all remaining rows and CommandComplete until every participating
application reports receiving the first chunk. Context, blocking, both Runtime
schedulers/layouts and native libpq pass this barrier; it is not inferred from
coalesced responses, process exit or kernel TCP acknowledgment.

Combined expanded evidence passes **132 successful Weave processes**, **12 native
libpq controls**, **2,082 synthetic connections**, **306 withheld-tail prefix
observations**, **36 affected CTest entries** and **six real-server processes**.
Exact inventories/JUnit agree with no failures, errors, skips or empty selections.
Every synthetic session checks exact simple-query/termination order. Real fixture
certificates are confirmed removed. Native project references/Ninja link commands
confirm libpq-only linkage and no sanitizer instrumentation; Weave's checks remain
enabled. Four production files differ from the qualified Flush checkpoint, and
their bytes are identical across all five prototype attempts.

| Ignored Archive | SHA-256 |
| --- | --- |
| postgres-exchange-chunks-prototype-v1-20261009.zip (failed fixture) | 8de98b5c57cafb027de29b6efec299c7519446b4b5e3100f09476802fe0227b3 |
| postgres-exchange-chunks-prototype-v2-20261009.zip (failed fixture) | 421f59888cf03f3c529619571e1d9c408c8fd51ea1b3b41b1e2856c4d26fd5d5 |
| postgres-exchange-chunks-prototype-v3-20261009.zip | 269f92a4a871a9628d38042a4a1e7a43aa3a8d38e5d74628ac538b515be30472 |
| postgres-exchange-chunks-prototype-v4-20261009.zip | 5141989009845e4465f869adf5ffcf82c3875a91d974391e0025ea391672d0c7 |
| postgres-exchange-chunks-prototype-v5-20261009.zip | 615c8804de087bd8c4ef9b9a9c326e864597fb69fa6110ad1a209871dbb1f07d |

Independent verification checks archive CRC/member hashes, frozen inputs, exact
mode/frame/process counts, withheld-tail barriers, native dependencies,
inventory/JUnit agreement, real positives/fixture cleanup and unchanged HEAD,
staged patch and protected ASan runtime. Permanent tests are not added yet:
focused empty-schema/binary expansion, permanent promotion, reduced-module and
relocated component packaging remain pending. Active ignored exploratory sources
are retained for that work and must be archived/removed before commit. No stage,
commit, push, upload, timing benchmark or new whole-module/security qualification
is implied. The callable audit keeps PQsetChunkedRowsMode Partial until promotion.

## Standalone Exchange Chunks Qualification (2026-10-09)

The pending promotion gates above now pass. Focused development v6 adds
zero-column RowDescription/DataRow sequences, copied table/attribute/type-size/
modifier/format metadata, binary values containing NUL/non-UTF8 bytes, UTF8 field
names/text, malformed CommandComplete after a delivered partial chunk and
cancellation before starting a deferred reader with CommandComplete pending.
The latter leaves the session usable and allows an uncancelled reader to finish;
observed cancellation after active reading remains terminal. Blocking controls
now also cover multi-statements, SQL error recovery, mixed COPY/query events,
adaptive retention, oversized rows and terminal malformed/EOF responses.

This expanded prototype passes Windows Debug and Linux ASan with installed
consumers, four-worker runtimes and native libpq 18.4/18.6 controls. It records
58 successful Weave processes, four native controls, 802 synthetic connections,
102 withheld-tail observations, 12 affected CTest entries and two real-server
processes. No production code changed during edge-case expansion. Binary format
fixtures exercise decoder and owning-chunk preservation against native libpq;
they do not imply a binary-result option for simple SQL queries. Zero-column
queries, SQL recovery, COPY and pending-completion cancellation also run against
actual PostgreSQL over plain TCP and verified mTLS/SCRAM channel binding.

After confirmation, exchange_chunks.cpp, exchange_chunks.py and the independently
linked exchange_chunks_libpq.cpp become permanent regressions. The same C++ source
provides an optional real-server executable; Runtime-dependent paths are guarded
so postgres-only consumers do not acquire Runtime, LDAP or GSS dependencies.
Native tests remain opt-in under WEAVE_POSTGRES_RESULT_LIBPQ_TESTS, with no Weave
linkage or sanitizer instrumentation. Library-only builds do not acquire libpq or
Python. Matching Windows dependency runtime files use existing target helpers.

Final frozen-source qualification passes all nine selected profiles:
Windows/Linux Debug, Release and ASan, Windows/Linux without Runtime or LDAP,
and Linux without Runtime, LDAP or GSS. Runtime profiles use four workers and
16 independently owned sessions under both schedulers; Windows exercises both
IOCP layouts, while Linux uses io_uring. Reduced profiles retain Context and
blocking coverage. Linux ASan retains leak detection.

The independent verifier confirms **83 CTest cases**, comprising eight affected
regressions per profile, eight optional native CTest entries and three component
packaging checks. Exact nonempty inventories and JUnit agree with no failures,
errors or skips. Streaming-specific controls record **207 successful Weave
processes**, **16 native libpq processes**, **2,455 synthetic connections** and
**314 withheld-tail observations**. The peer releases the query tail only after
all participating applications report receiving their first chunk; exact
simple-query/termination sequences are checked for every session.

All nine profiles separately pass real-server plain/mTLS controls. Owned server
and certificate fixtures are removed after each run. Relocated installs,
standalone headers and network-free symbol consumers pass Windows Release,
Linux Debug and Linux minimal component-packaging gates; consumers retain the
new exchange signatures and instantiate RowOptions calls in both facades.
Native project references/Ninja commands prove libpq-only linkage and disabled
exceptions/sanitizers. Production bytes match the confirmed development
implementation throughout; HEAD, the staged patch and protected ASan runtime
remain unchanged.

| Ignored Archive | SHA-256 |
| --- | --- |
| postgres-exchange-chunks-prototype-v6-20261009.zip | 764964304613b1a1389340ea45f812baf591ab622e5a8fd80a6cc51c83b95bd3 |
| postgres-exchange-chunks-permanent-v1-20261009.zip | d2e1f106e0579f1ad2dbce719a3ccbe2f70bca3f050b0cd493a02658c114fc4e |

Archive CRC/member hashes, source provenance, mode/frame/count assertions,
inventory/JUnit agreement, dependency boundaries and fixture cleanup are checked
independently. Earlier fixture failures remain retained and attributed above.
Exploratory primary sources are archived and removed after this promotion;
no staging, commit, push, upload or timing measurement is performed.

PQsetChunkedRowsMode moves from Partial to Mapped with explicit API differences:
RowOptions is selected before exchange submission, chunk kind is row_chunk, size
one uses the same kind, and the terminal zero-row tuples result carries the actual
command tag. The audit is now **127 Mapped, 12 Partial, four Open, 42 Different
and eight External**, not a completion percentage. Exact allocation reporting,
remaining configuration/result capabilities, deployment/security review and
final matched libpq measurements remain required by the full objective.

## Result Storage Development (2026-10-09)

Exact allocation reporting now has a separate owning-storage prototype and
result-shaped graph model. Three successive six-profile runs pass 36 CTest
entries, 18 storage-control processes and 18 independent native libpq processes.
The final model covers backing-request accounting, retained growth/shrink,
escaped strings/rows, independent clones, foreign pools, shared concurrent
allocation/destruction, immutable concurrent copies, alignment, external observer
ownership boundaries and standard-container interoperability idioms.

An injected backing allocator records actual request sizes/alignment and frees
independently of the pool counters. Reported pool/graph bytes agree with its live
request ledger; every observed request is released after final-owner destruction.
Native 18.4/18.6 controls establish retained-result allocation semantics, not
identical byte counts. Native linkage and exception/sanitizer boundaries,
archive CRC/member hashes, exact inventory/JUnit agreement and frozen/protected
inputs are independently verified.

[Storage design, exact archives and remaining integration gates](postgres-result-storage.md)
document the boundary: pool footprints can include shared or moved-out storage;
opaque observer owners/application allocations, caller storage and query scratch
are excluded. No platform heap-size or capacity estimate is used. The current
graph query uses a temporary deduplication set; public storage aliases and query
policy still need integration review. This is not the actual ResultSet engine,
a public allocator contract, permanent regression promotion, real-server/package
qualification or a timing benchmark. PQresultMemorySize remains Open, and the
callable audit remains 127 Mapped, 12 Partial, four Open, 42 Different and eight
External. No stage, commit, push or upload is performed.

## Result Storage Integration Development (2026-10-09)

Owning counted storage is now wired into the actual ResultSet, wire parsers,
buffered/outcome/portal/replication result paths, standalone exchange chunks,
pipeline deliveries, selective copies and result-local event records.
`memory_size()` reports retained backing requests for distinct reachable pools
and the local event node. Shared receiver/application state, caller allocation
and inspector scratch are excluded. Logical retained-data limits remain distinct
from physical pool footprint. The concrete public container types/ABI change;
full source compatibility is not claimed.

Schema/row pools are separate. Exchange publication releases its staging row
allocator instead of clearing a long-lived pool. Pipeline reservation can publish
an earlier batch; a prepared row then switches to the new batch's storage.
Copy assignment retains the fixture backing provider rather than mixing its row
storage into an unrelated default provider.

An initial six-profile integration sweep passed explicit-provider allocation and
streaming controls. A subsequent default-container control found an uncounted MSVC
Debug iterator-proxy pool. Proxy-enabled allocators now establish an owning shared
pool before rebinding; moves retain source handles until source reset. The failure
is preserved, and the latest six-profile sweep passes after the correction.
Release allocation remains lazy. This is an accounting fix, not a performance claim.

Independent verification checks 508 frozen first-party files, archived input CRCs
and member hashes, executable hashes, unchanged protected state and exact nonempty
CTest inventories/JUnit. The latest candidate passes **22 CTest cases**, including
six actual-library ledger controls and 16 affected full-feature Debug regressions.
**30 streaming processes** exercise **300 independent sessions** and
**307,200 rows** with bounded chunk footprints, owned copies/escaped data and
serialized lifecycle observations. Runtime controls use four workers/16 roots,
both schedulers and both Windows IOCP layouts. Context/blocking use one root.

Windows Debug/Release/ASan and Linux Debug/Release/ASan storage controls pass;
Windows ASan is Release, Linux ASan is Debug with leak detection. These scratch
builds disable optional GSSAPI/LDAP. They are not runtime-disabled/package gates.
Full-feature Windows/Linux Debug regressions cover results, transaction status,
pipeline status, Flush, exchange chunks, events, attachment and the module suite.
Loaded/header OpenSSL identity is checked in the new probes: 3.6.5/3.5.5.
The actual-library ledger covers full/selective/copy-assigned/foreign/escaped
storage and four-thread immutable copies. Its borrowed provider remains an
internal fixture, not a public allocator extension point.

Ignored evidence is retained in postgres-result-memory-integration-v1-20261009.zip
and postgres-result-memory-integration-verification-v1-20261009.json. Earlier
configuration/fixture failures and the real proxy defect remain diagnostic evidence.
No permanent test promotion, fresh native-libpq run, real-server/relocated-package
qualification, timing benchmark or whole-module production readiness is claimed.
All-producer accounting, compatibility/query-policy review and fault guards remain
before locking the API and promoting permanent regressions.

PQresultMemorySize advances from Open to **Partial**, not Mapped: the current
audit is **127 Mapped, 13 Partial, three Open, 42 Different and eight External**.
Remaining parity/deployment/security work and final matched libpq measurements
stay in scope. Nothing is staged, committed, pushed or uploaded by this checkpoint.

## Result Storage Compatibility Development (2026-10-10)

The actual result-text adapter now supplies standard string-style formatting and
hashing. Before the correction a real MSVC compilation failed: C++23 range
formatting rejected string width/precision, and std::hash<ResultText> was disabled.
The specialization forwards the standard string-view formatter/parser and hash,
including embedded NUL bytes; it is not a new formatting engine, a throwing Weave
operation or restoration of std::string ABI/exact-type compatibility. Private
ResultStorage creation now validates both backing callbacks before invoking them.

Independent post-correction verification confirms **18 CTest cases** across
Windows/Linux Debug, Release and ASan: six injected request/free ledgers, six
17-check interoperability processes and six fatal-guard drivers. Each guard driver
checks ten expected aborts plus one valid allocation/free control, for **60 expected
contract failures**. Coverage includes pool/block/copy allocation failure, missing
callbacks, multiplication/addition overflow, invalid/zero alignment and double
deallocation. There is no claim that every physically inaccessible counter-overflow
branch is exercised or that allocation failure returns a recoverable Result.

The owning streaming sweep passes again: **30 processes, 300 synthetic sessions
and 307,200 rows**. Runtime uses four workers/16 roots, both schedulers and both
Windows IOCP layouts; Context/blocking use one root. All retain the 2,048-byte
logical data bound, separately owned row batches and callback/copy/lifetime checks.

Six separate owned PostgreSQL 18 server runs cover actual buffered/outcome,
extended/prepared/description, portal, batch, pipeline, COPY OUT completion,
mixed-exchange, reset and retained-failure results. Every profile records
**2,050 inspections and 33,630 checks** (12,300/201,780 total). Context/blocking
exercise plaintext and verified mTLS/SCRAM-PLUS; both schedulers each run 32
independent roots on four workers. A reset can create further connections; these
are root counts, not a total connection census. The fixture creates/stops/removes
its own PostgreSQL cluster and credentials, never an existing service/data directory.

The producer oracle walks distinct pool identities on independent deep copies of
actual delivered results, compares graph totals and preserves original footprint,
metadata and lifetime. It is deliberately distinguished from the separately
injected backing-request ledger; wire-produced roots are not globally redirected
through a test allocator. Dedicated replication, standalone raw-row and pressure
accounting controls remain. Header/loaded OpenSSL identity is checked in the new
producer and storage probes: Windows 3.6.5, Linux 3.5.5. Windows ASan uses Release;
Linux ASan uses Debug with leak detection. Optional GSSAPI/LDAP are disabled in
the scratch builds, and provider dependencies are not all sanitizer-instrumented.

Windows Release and Linux Debug relocated component packaging also pass, giving
**20 CTest cases** in this checkpoint. Those isolated builds/public-header/consumer
checks do not establish runtime-disabled or full optional-provider qualification.
Archive CRC/member hashes, 508 common frozen first-party source hashes, executable
hashes, exact nonempty inventories/JUnit and protected HEAD/index/ASan state are
independently checked. Inputs are frozen per command; later scratch producer inputs
are archived separately from the earlier interoperability phase. The original
format/hash compilation failure is preserved, not counted as a passing case.

Ignored evidence: postgres-result-memory-compat-v1-20261010.zip and
postgres-result-memory-compat-verification-v1-20261010.json. Primary scratch sources
are archived and deleted after verification. Only the result-storage implementation
header changes production behavior in this checkpoint; no native-libpq or timing
run, permanent promotion, staging, commit, push or upload is performed.

PQresultMemorySize remains **Partial**. The audit stays **127 Mapped, 13 Partial,
three Open, 42 Different and eight External**. The public concrete-type/ABI and
temporary-query-allocation contract, remaining producer controls, permanent/full
qualification, wider deployment/security work and final matched libpq measurements
remain part of the original objective.

## Result Producer Ownership Development (2026-10-10)

Dedicated pressure-pipeline and raw-row/physical-replication memory probes pass
Windows/Linux Debug, Release and ASan. Production source is unchanged from the
compatibility checkpoint; new evidence confirms ownership rather than inventing
an unnecessary networking change. Independent verification checks six nonempty
CTest inventories/JUnit, six owned real-server fixture processes, archived inputs,
508 common frozen first-party sources and executable hashes. Protected HEAD,
the staged patch and the existing ASan runtime remain unchanged.

Pipeline probes complete **384 independent streams**, plus **six abandoned queued
delivery controls**, for **390 synthetic connections**. They include **84 pressure
streams yielding 1,572 early chunks**, **132 queued/byte cancellations**, and
**66 failed consumers**. SQL errors and abort recovery, oversized rows, split and
duplex drivers retain their existing assertions. The pressure fixture requests
37-row chunks under a 1,024-byte logical bound, requiring smaller early delivery.
Every row/value belongs to its delivered batch's distinct pool; previously held
pool footprints remain fixed while later chunks arrive, including prepared-row
reservation transitions. All old pools are deliberately retained by the probe,
so these checks do not claim a global physical quota for application-held results.
Windows Debug proxy/container sizes cause smaller early batches than Release;
RowOptions remains an upper bound, never an exact delivery count.

Real PostgreSQL 18 probes complete **168 raw-row streams**, retaining **21,504 rows**,
and **168 physical-replication streams**. Raw rows and copied column metadata
survive later reads, reuse, reset, finish and connection destruction; an escaped
text remains valid after retained rows/arena handles die. NULL/empty/long text and
ordinals are checked. Replication preserves both actual START_STREAMING and
START_REPLICATION completion results and retains moved-out command text through
reset/destruction/final result cleanup. The raw-row column check owns a copy;
it does not extend a borrowed row_columns reference's lifetime.

Context and blocking paths use plaintext and verified mTLS/SCRAM-PLUS. Runtime
probes use four workers, both schedulers and both Windows IOCP layouts; Linux is
sharded only. Pipeline runtimes use 16 independent roots per configuration;
real-server runtimes use eight. A reset creates further connections, so real
stream counts are not advertised as a total connection census. Each owned fixture
creates/stops/removes its own primary/standby clusters and credentials. Loaded/header
OpenSSL versions agree (3.6.5 Windows, 3.5.5 Linux). Optional GSSAPI/LDAP are off;
Windows ASan is Release, Linux ASan is Debug with leak detection. Dependencies
are not all sanitizer-instrumented.

Independent graph walks on deep copies check **3,702 pipeline** and **23,016
real-server result inspections**, with **2,285,124 total checks**. Row-pool identity,
fixed retained footprints and post-owner lifetimes are additional controls, not
global interception of every wire producer's backing requests. The injected
heap ledger remains separate. No fresh native-libpq run, packaging rerun or timing
benchmark is claimed by this checkpoint.

Initial fixture builds failed because of an incorrect certificate-header name
and a missing weave/port.hpp include; the reports/logs are retained with attribution.
An exploratory passing v3 run is not counted as qualification because arena
references outlived temporary allocator objects. v4 copies owning handles by value,
and the entire six-profile sweep passes after correction. Production calls use
full-expression references, and earlier memory probes use owning values or
full-expression calls; this probe defect does not demonstrate a production defect.

Ignored evidence is in postgres-result-memory-paths-v1-20261010.zip and
postgres-result-memory-paths-verification-v1-20261010.json. Inputs for every command
are archived separately; exact source/member/binary hashes and JUnit agreement are
checked before primary ad hoc sources are removed. No staging, commit, push,
upload or permanent test promotion is performed. PQresultMemorySize remains Partial;
the audit stays **127 Mapped, 13 Partial, three Open, 42 Different and eight External**.
API/query-policy lock, permanent and reduced/provider/full qualification, wider
deployment/security work and final matched libpq measurements remain in scope.

## Result Container Contract Development (2026-10-10)

The public container/query contract is selected for permanent regression promotion.
memory_size walks fields, may allocate distinct-pool bookkeeping, excludes that
scratch, and samples pool counters individually rather than promising a globally
atomic aggregate across concurrently allocating foreign pools. Mutable graphs
require external coordination; allocation failure follows the existing fatal
exception-disabled policy. The concrete container/ABI change is intentional and
not a std::string/std::vector exact-reference or template compatibility promise.

An initial six-profile Runtime-omitted probe confirmed standard formatting/hash,
explicit conversions, views and basic operations, but reported mixed-string
concatenation and implicit substring ownership as unavailable. Result text now
implements string/view/literal/character concatenation and substr using ordinary
owning std::string outputs, including embedded NUL bytes. Output allocations do not
grow the source result's retained pools. Those changes are ergonomic, not an I/O or
performance redesign.

API review identified a separate hazard, not an executed undefined-behavior test:
implicit conversion to an owning standard container can make a temporary for an
existing const-reference API, leaving a lazy coroutine with a dangling borrow.
String/vector owning conversions are now explicit; static compiler guards reject
implicit value/reference conversion. Borrowed views and explicitly constructed
temporaries still require ordinary lifetime coordination. The pipeline's schema
cache explicitly performs its previously implicit owning vector copy. Its first
build failed at that call site; the captured error is attributed, and all final
gates rerun after migration. No protocol/quota/allocator-provider/cancellation or
scheduler behavior is intentionally changed.

The finalized candidate passes Windows/Linux Debug, Release and ASan with Runtime
neither built nor linked, proven by cache/project references and native link
commands. Each profile performs **30 text/list interoperability checks** and an
owned PostgreSQL 18 Context/blocking probe over plaintext and verified mTLS/SCRAM-PLUS.
These cover **24 raw-row streams**, **3,072 retained rows**, **24 physical-replication
streams**, **3,288 result inspections** and **289,440 producer checks**. The oracle
walks copied-result pools and verifies raw-row/replication lifetimes; it is not
global heap-request interception. Header/loaded OpenSSL versions agree (3.6.5/3.5.5).
Windows ASan uses Release; Linux ASan uses Debug with leak detection. Linux GSSAPI/
LDAP and Windows LDAP are off in scratch builds; Windows SSPI remains the platform
implementation. Dependencies are not all sanitizer-instrumented.

Final full-feature Windows/Linux Debug runs pass eight affected regressions each,
including the module suite's four-worker scheduling/I/O-layout/cancellation cases.
Windows Release and Linux Debug relocated packaging each pass one case. Together
with six reduced interoperability entries, this is **24 CTest cases**, independently
verified against exact nonempty inventories/JUnit. Earlier baseline/intermediate
runs are archived separately, not included in final totals. No fresh native-libpq,
whole Runtime six-profile storage sweep or timing measurement is claimed here.

Ignored evidence is in postgres-result-memory-contract-v1-20261010.zip and
postgres-result-memory-contract-verification-v1-20261010.json. Frozen input/source/
executable hashes, native dependency boundaries, 508 common first-party sources,
HEAD, the staged patch and protected ASan DLL are verified. Only three production
files change: the storage implementation header, the public metric comment and
the explicit pipeline-cache copy. Documentation changes happen after final gates.
Primary probes are archived and deleted; no stage, commit, push or upload occurs.

The implementation/API shape is now selected, but permanent/native/full release
qualification remains before mapping PQresultMemorySize as complete. The audit
stays **127 Mapped, 13 Partial, three Open, 42 Different and eight External**.
Remaining parity, deployment/security work and final matched libpq measurements
are still required by the original objective.

## Result Storage Permanent Qualification (2026-10-10)

The selected result-storage implementation now has permanent module-owned
regressions and the full nine-profile storage matrix: Windows/Linux Debug,
Release and ASan, both Runtime-omitted Debug builds, and Linux with Runtime,
GSSAPI and LDAP disabled. The eight enabled independent libpq controls pass
against Windows 18.4 and Linux 18.6. These are functional native controls;
system libpq/OpenSSL are not sanitizer-instrumented by these builds.

The final gate verifies 112 nonempty CTest cases with no failure/error/skip:
61 storage/native/real-server cases, 48 existing affected regressions across
the six full profiles, and Windows Release plus Linux Debug/Release component
packaging. Ten additional owned-server invocations complement eight server cases
inside CTest, for 18 disposable fixture processes total. Plaintext and verified
mTLS/SCRAM-PLUS, blocking/Context operation, four-worker schedulers and supported
Windows I/O layouts are exercised. No existing database service is used.

[Storage scope and exact workload totals](postgres-result-storage.md#permanent-regression-qualification-2026-10-10)
distinguish the independent request/free ledger from compositional copied-graph
inspection; they are not a whole-process heap oracle, memory benchmark or RSS claim.
Explicit owning conversions prevent implicit standard-container conversion
temporaries; explicit temporaries and borrowed views still need normal lifetime care.

The retained early Windows ASan failure is attributed to an unsupported runner
LeakSanitizer option, rejected before the instrumented test bodies ran. The complete
corrected sweep and regressions pass with platform-appropriate sanitizer settings.
Earlier passing/polish rounds are retained but not added to final totals.

Ignored evidence is in `postgres-result-memory-permanent-v1-20261010.zip` and
`postgres-result-memory-permanent-verification-v1-20261010.json`. All 519 current
first-party sources, exact frozen inputs, executable hashes, inventories/JUnit and
protected HEAD/index/ASan DLL are checked. Eleven permanent test/helper sources and
their CMake registration change; production runtime/network/storage code does not.
Documentation changes follow the final gates. Primary ad hoc qualification scripts
are archived and deleted; no staging, commit, push or upload occurs.

The callable inventory is now **128 Mapped, 12 Partial, three Open, 42 Different
and eight External**, not a completion percentage. Remaining functionality,
deployment/security work and final matched libpq allocation/performance measurements
remain required; this checkpoint does not certify the complete module for production.

## TLS Password Provider Development (2026-10-10)

Owning synchronous `TlsPasswordProvider` callbacks now integrate through typed
TLS options. PostgreSQL retains them before initial suspension and releases
selected credentials after construction, including dropped/rejected Tasks.
Connection attempts track credential setup failure separately from native
endpoint availability, preventing timeout/connection/target-session callback
errors from causing connect/reset/ping host retry. Nonsecret configuration
inspection records only provider presence.

[TLS checkpoint](tls-release.md#key-passphrase-provider-checkpoint-2026-10-10)
records six full development profiles, real mTLS/SCRAM-PLUS Context/blocking/reset/
four-worker controls, 240 zero-byte terminal-failure connections and 73 fresh
existing CTest/package gates. Temporary qualification sources and fixtures are
sealed then removed; production code, contracts and qualified source snapshots
remain. No timing runs, staging, commit or push occur. Permanent feature-specific
regressions, reduced builds and independent native-hook controls remain required
before changing the audit's Partial classification. Whole-module release and
final matched libpq allocation/performance measurements remain unfinished.

## Permanent TLS Provider Coverage (2026-10-10)

The [permanent TLS checkpoint](tls-release.md#permanent-provider-regressions-2026-10-10)
now qualifies owning factory/decoder/error/cleanup tests and provider-loaded
Context/four-worker mTLS on nine full/reduced profiles, with 18 nonempty CTest
cases. Its owned encrypted-key fixture removes extra key files before the base
certificate directory; tests assert complete directory removal. TLS test-only
sources/registration change, not the PostgreSQL or TLS production implementation.
Ignored evidence is `tls-password-provider-permanent-v1-20261010.zip` and its
verification JSON; primary qualification scripts are sealed and removed.

PostgreSQL's specific permanent terminal-failure/lazy-owner/reset/live controls
and independent libpq hook qualification remain pending. The callable audit
retains Partial and its previous counts; the TLS milestone does not replace
whole-module qualification or final matched measurements.

## PostgreSQL Key Password Provider Regressions (2026-10-10)

Permanent PostgreSQL tests now qualify the already implemented owning,
demand-driven encrypted-PEM password provider. Production source is unchanged
from the development and permanent TLS snapshots. The promoted tests retain
the confirmed callback-error and lazy-ownership contracts, rather than adding
libpq linkage or process-global hook state to Weave.

The exact final matrix contains **104 passing CTest cases** on nine profiles:
Windows Debug/Release/Release-ASan; Linux Debug/Release/Debug-ASan; Windows and
Linux without Runtime/LDAP; and Linux without Runtime/GSSAPI/LDAP. This comprises
37 focused provider cases, 63 existing affected regressions and four Windows/
Linux Release component-package cases. Eight Windows real-server invocations
run outside CTest. Together with the ten Linux CTest fixture invocations, there
are **18 owned primary/standby runs**, including nine isolated native libpq
password-hook controls. No existing database or service is used.

Coverage includes:

- TLS factory callbacks and provider-loaded TLS 1.2/1.3 mTLS on Context and all
  supported four-worker scheduler/I/O layouts.
- PostgreSQL SCRAM-PLUS over verified mTLS, blocking/Context startup and reset,
  and four-worker independent connections. Reduced builds keep blocking/Context
  controls without requiring Runtime.
- Failed callbacks carrying zero, timeout, connection-reset or target-session
  error codes remain failures through connect, blocking connect, ping and reset.
  Reset retains one failed attempt and closes the old connection; it does not
  retry the standby.
- Dropped and rejected unstarted connect/reset Tasks retain then release their
  provider owners. The existing session remains usable after an unexecuted reset;
  completed credentials retain neither providers nor returned passphrases.
- Independent two-endpoint peers count **288 connections**, all to the first
  endpoint, with zero protocol bytes, zero second-endpoint connections and no
  peer errors. This checks actual traffic, not only client-generated reports.
- Independent libpq 18.4/18.6 processes verify native hook registration/restoration,
  key-path lookup, valid/wrong/overlong/denied passwords, unencrypted-key behavior,
  reset and four-thread connections. Each records 37 callback requests.

These native controls link no Weave code. Their executables and installed
libpq/OpenSSL dependencies are not ASan-instrumented, even when paired with a
Weave ASan profile. Windows Weave ASan uses detect_leaks=0; Linux uses
detect_leaks=1. OpenSSL versions remain Windows 3.6.5 and Linux 3.5.5; the owned
server fixtures use PostgreSQL 18.6. This is functional compatibility evidence,
not identical native callback ABI, arbitrary provider/deployment coverage or a
security audit.

### Retained Failures And Final Assembly

All failed rounds remain in the evidence. An early Linux ad hoc build was
started before its configure completed; the dependent build/live commands did
not execute a library test. Corrected sequential commands passed.

The first permanent round exposed an existing TLS session-expiry test defect:
a one-second capture window could expire at a whole-second boundary. Under the
pinned exception-free doctest build, failed REQUIRE logs a failure but continues
execution; the test then dereferenced its failed Result. An intentional delayed
capture and a separate failing-REQUIRE probe confirm both conditions, including
the session_unavailable error value seen as address 0x6 in the ASan report. The
test now explicitly guards failed setup and allows three seconds for capture
before asserting real expiry. No production lifetime policy is weakened.

The second round exposed a promoted test's Runtime-only local variable used in
a reduced build. Moving that test constant outside the conditional fixes it.
The third round's child commands passed 102 CTest cases, but its exact inventory
guard correctly rejected two absent opt-in server registrations in the minimal
profile. Enabling those registrations and running exactly those two controls
completes the matrix on identical first-party bytes. The incomplete outer round
is not relabeled successful, and no failed sample is silently discarded.

### Evidence And Remaining Scope

Ignored evidence is `postgres-tls-password-permanent-v1-20261010.zip` and
`postgres-tls-password-permanent-v1-20261010-verification.json`. Verification
checks exact JUnit inventories, every final command's exit/source boundary,
archived input hashes, native/Weave control output, synthetic-peer traffic and
the prior permanent TLS snapshot. First-party production matches that snapshot;
only tests, test registration, the failed-REQUIRE development rule and checkpoint
documentation change. Owned fixture cleanup is checked for reported paths;
live Weave certificate cleanup also relies on the fixture's normal-exit RAII.
Temporary primary investigation sources/scripts are archived and then removed.
HEAD, staged bytes and the protected compiler ASan DLL remain unchanged.

`PQsetSSLKeyPassHook_OpenSSL` is now an explicit **Different** callback model:
Weave provides the qualified per-credential capability, not process-global
registration or a PGconn/native-engine callback ABI. The callable inventory is
**128 Mapped, 11 Partial, three Open, 43 Different and eight External**, not a
completion percentage. Native key engines, other credential formats, remaining
connection-option/deployment gaps, sustained release/security qualification and
final matched libpq allocation/performance measurements remain required. No
benchmark, staging, commit, push or upload is performed.

## Direct TLS Development (2026-10-10)

Direct negotiation is implemented through Options::tls_negotiation, OptionsInfo,
the shared keyword/environment/service schema, startup and independent cancellation
snapshots. The default remains PostgreSQL SSLRequest negotiation. Direct mode uses
per-handshake required `postgresql` ALPN without changing a shared TlsContext;
plaintext/direct is rejected and verification remains mandatory. Session offers
are bound to the same required label before their one-shot state is consumed.

An independent peer reproduced a genuine development defect: GSS `prefer` followed
by `N` initially sent ClientHello on the GSS negotiation socket. PostgreSQL checks
direct TLS at initial connection entry, so that socket cannot be reused. The fix
closes it and connects to the captured endpoint, reapplying native TCP controls.
Fresh setup and handshake failures remain terminal. Required GSS never falls back;
successful GSS still takes priority. No TCP/runtime scheduler redesign is involved.

### Confirmed Development Controls

Six Windows/Linux Debug, Release and Weave-ASan profiles pass on identical
first-party bytes. These are sequential development commands, not CTest or final
release qualification. Controls cover:

- TLS 1.2/1.3, both credential roles, binary/exact ALPN, stateful/ticket resumption,
  non-consuming policy mismatch and ordinary shared-credential behavior.
- Prebuilt/file mTLS startup, SCRAM-PLUS, reset, actual backend cancellation with
  SQLSTATE `57014`, query reuse, blocking operation and four-worker execution.
- Independent TLS peers with missing/unrelated ALPN, untrusted CA, hostname
  mismatch, failed reset and rejected cancellation handshakes. TLS 1.2 peers
  observe zero application bytes after policy rejection; TLS 1.3 peers record
  abort before native accept completes, not a successful application handshake.
- Endpoint-pinned OAuth discovery/token reconnect and reset for both TLS versions,
  plus rejection before token delivery when reconnect lacks ALPN. Independent
  secondary endpoints observe no connections.
- GSS decline with fresh direct TLS and required-GSS no-fallback controls across
  Context, blocking and four-worker schedulers, including both Windows I/O layouts.
- Independent libpq 18.4/18.6 real-server direct/legacy mTLS, reset, cancellation
  and reuse controls, in processes that do not link Weave.

Separate Linux Debug/Release/ASan controls own disposable KDC/PostgreSQL fixtures.
With server TLS disabled, GSS `prefer`/`require` still establish protected transport
under direct-TLS configuration; both session metadata and pg_stat_gssapi verify
encryption. Context, blocking, reset and both four-worker schedulers pass, alongside
independent psql/libpq controls. This is not Windows domain/Kerberos qualification.

The combined verified inventory is 61 final commands, 15,732 repeated functional
checks and 2,376 audited synthetic connections, with 12 owned TLS fixture runs
and three owned Linux GSS fixtures. These counts are not distinct features or
performance measurements. Linux Weave ASan retains leak detection; Windows does
not. Native libpq/OpenSSL/GSS dependencies are not all sanitizer-instrumented.

### Retained Failures And Evidence

All failed/incomplete rounds remain archived. An initial Windows configure split
an unquoted CMake path; a development move-assignment failed because TcpStream is
move-constructible only; the replacement uses variant emplacement. Probe-only
errors included a nonexistent error enumerator and a TLS 1.3 peer expecting native
accept to finish after the client had already rejected ALPN. The corrected peer
retains the precise native EOF observation and adds completed TLS 1.2 controls.

The independent Windows baseline initially failed before publishing certificates:
without app-local libpq runtime deployment, PATH selected PostgreSQL 16's DLL.
A controlled reproduction records `0xc0000138` (missing ordinal), versus
`0xc0000135` when libpq is absent from an isolated PATH. Deploying the pinned
runtime lets the same executable enter main; complete version-checked real-server
controls pass. This is a comparison-fixture loader failure, not a Weave failure.
Failed attribution probes are retained rather than hidden or mislabeled passing.

Ignored archive `postgres-direct-tls-development-v1-20261010.zip` has SHA-256
`011d86bb7cfc7ab2edf4f398f9c19611783d990e45b10437e5c6a7f0a5a55c3b`;
the accompanying verification JSON checks all 527 first-party inputs, every final
command/source boundary, archived hashes and structured peer/native results.
Exactly nine production files differ from the preceding password-provider snapshot.
Exploratory primary sources/scripts are sealed in the archive and then removed.
HEAD, staged bytes and the protected compiler ASan DLL remain unchanged.

### Pending Promotion

At this development checkpoint, permanent direct-TLS regressions, updated legacy option-schema expectations,
fresh-reconnect refusal controls, existing release suites, reduced profiles and
relocated packaging remain pending. Earlier qualified snapshots do not qualify
this changed source. Callable audit counts remain 128 Mapped, 11 Partial, three
Open, 43 Different and eight External. Broader parity, deployment/security review
and final matched measurements remain open. No benchmark, staging, commit, push
or upload is performed.

## Direct TLS Regression Checkpoint (2026-10-10)

The confirmed required-ALPN engine and endpoint-pinned GSS-decline reconnect
behavior now have permanent regressions. This checkpoint changes only test
sources/registration and documentation, not library code or networking policy.

- `weave_tls_alpn` covers TLS 1.2/1.3, exact and binary labels, both roles,
  actual stateful/ticket resumption, non-consuming session-policy rejection,
  missing/different selection, overlong labels and ordinary credential behavior.
- `weave_postgres_tls_reconnect` uses an independent peer that closes its listener
  before sending GSS `N`. Fresh TCP refusal must remain a terminal TLS-stage
  failure on the selected endpoint. The original socket must receive no further
  bytes, and an available second host must receive no connections. Context,
  blocking, both ping facades and four-worker runtimes cover both schedulers and
  every supported I/O layout.
- Parser, environment/schema and owning configuration tests now include
  `sslnegotiation`, verified/default policy, invalid plaintext combinations and
  retained snapshots. Nine isolated native libpq option-schema controls pass;
  these are descriptor comparisons, not new direct-TLS server qualification.

Windows/Linux Debug, Release and ASan, Windows/Linux without Runtime, and Linux
without Runtime/GSS/LDAP pass **103 regression entries plus four TLS/PostgreSQL
component-package entries**. All CTest selections use `--no-tests=error`, and
exact nonempty JUnit inventories have no failures, errors or skipped entries.
The GSS-dependent reconnect test is explicitly absent when GSS is disabled;
ALPN/options and the independent native schema control still run in that profile.
The package gates include relocated consumers and standalone public headers.

A further **17 entry passes** rerun only ALPN/reconnect selections with complete
passing-output retention. Their structured peer audits verify 176 accepted GSS
negotiations, empty old-socket tails and zero second-host connections. These
supplements repeat features, not add 17 distinct capabilities. The preceding
six-profile development refusal sweep separately audits 168 connections. Linux
ASan retains leak detection; Windows does not. Third-party dependencies are not
all sanitizer-instrumented.

### Retained Setup And Evidence Failures

The first refusal fixture expired all unused listeners after ten seconds.
Windows refused-connect controls take roughly two seconds each, so later batches
reached already-closed initial endpoints and legitimately entered ordinary
initial-availability failover. Reports identify the initial transport stage,
not a failed post-GSS TLS reconnect. The corrected fixture keeps unvisited
listeners alive until owned-process completion, and recorded accepts after ten
seconds confirm why the original expiry was invalid. Production deadlines and
host-selection policy are unchanged. Both failed probe rounds remain archived.

The initial permanent sweep passed eight profiles and packaging, then stopped
because the final reduced profile did not enable its optional native comparison
target. Enabling that test-only option completes the same eleven-entry selection
on identical first-party bytes. The original outer sweep remains **failed**;
the combined success inventory derives from independently checked JUnit records.

The first seal attempt rejected truncated structured JSON: CTest retained only
1 KiB of passing output. The focused supplement raises that output bound to
64 KiB, verifies every complete audit and changes no library/permanent test code.
This was missing detailed evidence, not a failing library test.

Ignored archive `postgres-direct-tls-regression-v1-20261010.zip` has SHA-256
`180d6f8f711e7eb0becd18eef1e9b17858927b3f12b55d7a27f3ea305b4b4c4c`.
The verification sidecar checks 530 first-party files, 59 qualified command
boundaries, complete input/archive hashes and all exact JUnit inventories.
Development/promotion/completion/output-retention phases have distinct primary
script snapshots; first-party bytes are identical across promotion and its
supplements. Nine test/CMake files differ from the development snapshot, with
zero library-source changes. All exploratory primary sources/scripts are archived
and removed afterwards. HEAD, staged bytes and the protected ASan DLL are unchanged.

### Remaining Direct TLS Work

Permanent PostgreSQL startup/reset/cancellation/OAuth controls, real-server and
protected-GSS priority regressions, the broader PostgreSQL release selections
and deployment/security review remain pending. Existing development controls
are not silently relabeled permanent or full release qualification. No timing
benchmark, staging, commit, push or upload is performed in this checkpoint.

## Direct TLS Peer Regression Qualification (2026-10-10)

The confirmed synthetic startup/reset/cancellation/OAuth development controls now
have permanent `weave_postgres_tls_negotiation` coverage. Only its C++/Python
test sources and private CMake registration change; library sources remain
byte-identical to the preceding direct-TLS regression snapshot.

An independent Python SSL/PostgreSQL peer restricts each run to TLS 1.2 or 1.3.
Its eight modes cover absent/unrelated ALPN, untrusted chains, wrong hostnames,
rejected reset/cancellation handshakes, successful custom-provider OAuth with
reset, and rejected ALPN on OAuth reconnect. Exact client error categories,
retained direct-mode configuration and original-session cleanup are checked.
An available fallback listener must receive zero connections. Accepted sockets
start with a TLS handshake record, never SSLRequest; rejected handshakes expose
no protected Startup or token work to the peer. Native handshake-aborted EOF
observations are retained separately from completed TLS handshakes.

Context and blocking cases cover every mode. Four-worker runtimes cover startup
rejection and successful OAuth startup/reset on both schedulers and every
supported I/O layout: Windows sharded/shared IOCP and Linux sharded io_uring.
The deliberately rejected reset/cancellation/OAuth-reconnect cases stay serial
so each fresh handshake has a deterministic paired original connection.
Runtime-free profiles retain all Context/blocking modes; disabling GSS/LDAP
does not disable direct TLS or introduce a Runtime dependency.

| Profile | New peer scenarios | Audited connections | Client checks | Provider calls |
| --- | --- | --- | --- | --- |
| Windows Debug, Release, ASan, each | 16 | 568 | 2,150 | 140 |
| Linux Debug, Release, ASan, each | 16 | 312 | 1,202 | 76 |
| Windows/Linux without Runtime, Linux without Runtime/GSS/LDAP, each | 16 | 56 | 254 | 12 |
| Total across nine profiles | 144 | 2,808 | 10,818 | 684 |

The focused selections pass **53 nonempty regression entries plus four relocated
TLS/PostgreSQL component-package entries**. They include existing required-ALPN,
OAuth startup, cancellation, PostgreSQL core and GSS-decline reconnect regressions
where supported. Every selection uses `--no-tests=error`, exact JUnit inventories
and full passing-output retention; no failures, errors or skips occur. Complete
JSON peer audits independently verify the table, zero fallback accepts, zero live
peer threads and removal of the reported fixture files. No failed round is omitted
or relabeled in this sweep. Existing compiler warnings in unrelated OAuth tests
remain visible in the logs.

Linux Weave ASan enables leak detection; Windows disables it. Native dependencies
are not all sanitizer-instrumented. These synthetic peers do not qualify real
SCRAM-PLUS, native OAuth device-flow deployments or positive Kerberos servers.

Ignored evidence `postgres-direct-tls-peer-permanent-v1-20261010.zip` has SHA-256
`f58192f2f309704c42c611bfd3dfc484a8adf8c0781582455c2525871be289f1`.
Its verification sidecar checks 532 first-party files, all 30 sequential command
boundaries, complete input/archive hashes, all 57 exact JUnit entries and every
structured peer audit. Library sources, HEAD, staged bytes and the protected
ASan DLL remain unchanged. Three scratch automation sources are sealed and then
removed; no benchmarks, staging, commit, push or upload occur.

Permanent owned real-PostgreSQL direct-TLS/mTLS/SCRAM-PLUS startup/reset/actual
query-cancellation controls, independent direct/legacy libpq controls and
protected-GSS priority regressions remain next. Broader release selections,
deployment/security review, remaining parity capabilities and final matched
performance/allocation measurements are still required. The callable inventory
stays 128 Mapped, 11 Partial, three Open, 43 Different and eight External; these
counts are not a completion percentage or whole-module production certification.

## Direct TLS Server Regression Qualification (2026-10-10)

Permanent `weave_postgres_tls_negotiation_live` now qualifies direct-TLS startup,
reset and observed query cancellation against owned PostgreSQL 18.6 primary/
standby fixtures. Both file-loaded and prebuilt credentials use verified mTLS
and mandatory SCRAM channel binding. Owning client metadata checks actual
`postgresql` ALPN and completed SCRAM; backend `pg_stat_ssl` independently
confirms TLS and a client certificate. Every asynchronous session cancels an
executing notice-producing query, checks SQLSTATE `57014`, then successfully
queries again. Blocking cancellation uses an independent owning snapshot on a
separate caller thread and waits for the target backend's actual active `PgSleep`
state before dispatch. The query connection never crosses threads.

Four-worker runtimes cover both schedulers and every supported I/O layout with
16 simultaneous sessions per combination, including actual query cancellation
and post-cancellation reuse. Windows covers sharded/shared IOCP; Linux covers
sharded io_uring. Runtime-free profiles retain file/prebuilt Context and blocking
controls; the minimal Linux build additionally disables GSSAPI and LDAP.

| Profile | Weave client checks | Observed Weave query cancellations | Independent libpq cancellations |
| --- | --- | --- | --- |
| Windows Debug, Release, ASan, each | 1,858 | 132 | 2 |
| Linux Debug, Release, ASan, each | 958 | 68 | 2 |
| Windows/Linux without Runtime, Linux without Runtime/GSS/LDAP, each | 58 | 4 | 2 |
| Total across nine profiles | 8,622 | 612 | 18 |

The recorded client-check totals include the blocking observer's active-state
checks; extra polls can change that assertion count without changing scope.
Cancellation counters advance only for dispatched requests whose corresponding
query/reuse checks also pass; they are not a guarantee made by CancelHandle's API.

`weave_postgres_tls_negotiation_libpq` is an independent executable linking no
Weave libraries. Nine isolated processes compare both `sslnegotiation=direct`
and legacy `postgres` startup/reset/cancellation with the same verified mTLS and
required channel-binding policy. They verify loaded/header OpenSSL identity and
the exact configured libpq version: Windows 18.4/OpenSSL 3.6.5 and Linux
18.6/OpenSSL 3.5.5. Private Windows app-local DLL copies avoid ambient libpq
resolution; nothing changes the user's global installation. These controls make
356 native assertions and observe 18 canceled queries with successful reuse.

Permanent `weave_postgres_tls_gss_priority` additionally passes Linux Debug,
Release, ASan and without Runtime. Four disposable KDC/PostgreSQL fixtures set
server TLS **off**, use real Kerberos mutual/encrypted transport, and retain
requested direct-TLS policy. Both GSS `prefer` and `require` must succeed on
Context, blocking and supported four-worker schedulers with no negotiated TLS;
backend `pg_stat_gssapi` confirms encryption. Independent psql controls verify
both policies in each fixture. The four runs make 792 client checks plus eight
native policy controls, drain their KDCs and confirm fixture-directory removal.
This does not qualify positive Windows domain/Kerberos deployments.

The frozen nine-profile selections pass **67 nonempty regression entries plus
four relocated TLS/PostgreSQL package entries**. Eight additional native Windows
real-server runs use owned WSL PostgreSQL servers; the Windows executables still
use their native IOCP backend. Together the nine Weave and nine libpq real-server
runs own 18 primary/standby fixtures. Four separate GSS fixtures own their KDCs
and single servers. Existing synthetic peer selections also re-audit 2,808
connections, including zero fallback accepts. Exact JUnit inventories, complete
passing-output retention and `--no-tests=error` prevent empty/truncated success.
No failed qualification round occurs in this sweep.

Library sources remain byte-identical to the preceding direct-TLS snapshots.
Only four permanent test sources, private CMake registration and documentation
change. Real-server tests require the existing opt-in `WEAVE_POSTGRES_SERVER_TESTS`;
the native target additionally requires `WEAVE_POSTGRES_TLS_LIBPQ_TESTS`, and
the KDC priority test requires Linux's existing `WEAVE_POSTGRES_KERBEROS_TESTS`.
None adds dependencies to library-only consumers. Linux Weave ASan enables leak
checking; Windows disables it. Native libpq/OpenSSL/GSS dependencies are not all
sanitizer-instrumented; independent native baselines are functional controls.

Ignored archive `postgres-direct-tls-server-permanent-v1-20261010.zip` has SHA-256
`9fa52eec3661419541542b40320ce6b0446a54bc6faff585cbc02dc23838338b`.
Its verification sidecar checks 536 first-party files, all 38 sequential command
boundaries, complete input/archive hashes, all 71 JUnit entries, the eight external
Windows runs and the full structured peer/KDC audits. HEAD, staged bytes and the
protected ASan DLL remain unchanged. Four scratch automation sources are sealed
then removed; no benchmarks, staging, commit, push or upload occur.

These gates complete the documented direct-TLS core integration promotion, not
whole-module release qualification, broader native OAuth/Windows domain deployment
coverage, independent security review or final performance/allocation measurements.
Remaining option/result capabilities and replication/deployment soaks still apply.
The callable audit stays 128 Mapped, 11 Partial, three Open, 43 Different and eight
External; the counts are not a completion percentage or drop-in libpq parity.

## SNI Policy Checkpoint (2026-10-10)

Client `TlsHandshakeOptions::server_name_indication` and PostgreSQL
`Options::server_name_indication` default to true. Disabling them suppresses DNS
SNI only: certificate-chain and DNS/IP verification remain mandatory. Session
offers bind the SNI policy and reject a mismatch before consuming the capture.
The per-handshake policy does not mutate shared credential snapshots. PostgreSQL
supports `sslsni=0/1`, service/URI loading and explicit `PGSSLSNI` loading;
OptionsInfo exposes the requested nonsecret policy. Both TLS negotiation modes,
reset, cancellation snapshots and endpoint-pinned custom-provider OAuth retain
their own policy. An older cancellation handle is not retargeted by reset.

The development controls found a Windows loader defect: a present-empty
environment variable was rejected when GetEnvironmentVariableW's second read
returned zero with no native error. That case now preserves defaults, matching
Linux; real native errors and oversized/racing reads remain rejected. Ten library
files change relative to the preceding direct-TLS checkpoint.

### Qualified Scope

- Permanent Windows Debug, Release, ASan and without Runtime each pass the exact
  15-test selection: **60 nonempty regression entries**, plus **two Release
  TLS/PostgreSQL component-package entries**. Packaging includes isolated builds,
  relocated consumers and public-header probes.
- Five development profiles pass four entries each: Windows Release, ASan and
  without Runtime, plus Linux Debug and Release. Those **20 entries** are separate
  from the permanent promotion, not evidence for missing Linux permanent gates.
- Across these selections the structured audits contain 504 peer scenarios,
  20,160 Weave and 360 native connections, 32,148 engine checks, 60,264 client
  checks, 3,360 OAuth callbacks and 108 isolated loader controls. Counts aggregate
  development and permanent runs; they are not distinct production deployments.

Permanent `weave_tls_sni` observes actual ClientHello extensions through an
independent OpenSSL server. Both TLS versions, DNS/IPv4/IPv6 verification,
stateful/ticket resumption, one-shot/non-consuming policy failures and concurrent
shared credentials are covered. `weave_postgres_sni` uses an independent Python
SSL/PostgreSQL peer to inspect SNI on startup, reset, old/current cancellation
snapshots and protected OAuth discovery/reconnect. It checks serial connection
order, concurrent aggregate balances, rejected verification with zero fallback
accepts, protocol framing, drained handlers and owned fixture removal. A valid
synthetic CancelRequest is not proof of interrupting an executing SQL query.

Context, blocking and optional four-worker runtimes are covered. Runtime profiles
exercise both schedulers and supported layouts; Linux does not claim shared I/O.
Loader controls isolate ambient PG variables and verify service/environment/
keyword precedence and owned fixture cleanup. Native `weave_postgres_sni_libpq`
links no Weave library and requires the exact libpq version and matching loaded/
header OpenSSL identity. Qualified native versions are Windows libpq 18.4 with
OpenSSL 3.6.5 and Linux development libpq 18.6 with OpenSSL 3.5.5. Windows Weave
ASan disables leak detection; native baselines/dependencies are functional controls,
not instrumented simply because the surrounding profile is labelled ASan.

### Retained Failures And Evidence

An early Windows Debug development run passed assertions but loaded OpenSSL
3.6.4 against 3.6.5 headers. Its output is retained as diagnostic-only and excluded
from qualified counts. Permanent Debug adds explicit version checks and passes
with 3.6.5. The post-gate verifier revision rejects the earlier mismatch rather
than counting its green assertions; no library, permanent test or executed peer
changed for that revision. Command-specific input snapshots retain the original
verifier bytes, and the manifest records both verifier hashes and the reason.

Other retained setup failures are attributed: split PowerShell CMake path arguments,
a scratch helper colliding with std::quoted, the actual empty-environment loader
bug, overly strict fixture EOF handling, a native cancellation fixture waiting
for client EOF before supplying the server EOF, and an incorrect named build
target. Corrected complete Windows selections pass; no failed round is erased.

Linux ASan linking failed with native I/O errors before tests ran. The host C:
drive was observed full; WSL kernel diagnostics report an offline disk, journal
abort and emergency read-only filesystem. Only this checkpoint's newly generated
Windows build trees were relocated to E: to recover space. No distro repair or
restart is performed without approval. Linux ASan/reduced/minimal permanent
selections and Linux packaging remain unverified, not successful or library
failures inferred from a storage error.

Ignored archive `postgres-sni-checkpoint-v1-20261010.zip` has SHA-256
`c71600cf3bc7efc1b15380b39c61f75fd0e2b4445fd1fc061901a6f7caa52171`.
Its manifest/verification sidecar checks 552 source inputs, command snapshots,
archive hashes, exact nonempty JUnit inventories, native versions and complete
structured peer audits. There are 82 qualified test/package entries in total.
HEAD, staged bytes and the protected ASan DLL remain unchanged. Eleven primary
scratch sources are sealed then removed; curated documentation is updated after
sealing. No benchmark, staging, commit, push or upload occurs.

Remaining SNI gates include Linux permanent/package qualification, real-server
mTLS/SCRAM-PLUS/reset/actual-query-cancellation controls and public resumed-stream
adapter controls. This checkpoint neither replaces the prior default-policy
direct-TLS server evidence nor certifies whole-module production readiness.
Certificate-selection/CRL-directory/native-key capabilities, result gaps, deployment
soaks, independent review and final matched measurements remain in scope. The
callable inventory remains 128 Mapped, 11 Partial, three Open, 43 Different and
eight External; those counts are not a completion percentage.

## SNI Stream Adapter Regressions (2026-10-10)

The public ordinary/resumed `tls::client` factories now have permanent
`weave_tls_sni_streams` coverage over real IOCP TCP with an independent native
OpenSSL server engine. It checks both TLS versions and stateful/ticket modes,
DNS/IPv4/IPv6 verification names, SNI on/off, exact required ALPN, mTLS,
authenticated shutdown and actual resumption on both engines. Numeric IPv4 TCP
is used even for IPv6 verification names; this is not an IPv6 transport gate.

Each chain makes 11 connections. The first captures seven aliases of one session
and gracefully closes. Dropping an unstarted resumed Task does not claim that
session. Changed SNI, name, ALPN and credential-snapshot offers fail before TLS
bytes and leave a remaining alias available. A matching offer then resumes on
both Weave and native OpenSSL, consumes the one-shot claim and makes a duplicate
offer fail before bytes. Three ordinary-client controls reject a wrong DNS name,
wrong IP or untrusted CA with `certificate_verification`, even with SNI disabled.
Native peers observe actual ClientHello extension presence, verify mTLS, and
send no application data on verification rejection. No security/library fix was
needed; only a permanent test source and its private CMake registration change.

| Windows profile, per development/permanent run | Chains | Connections | Actual resumptions |
| --- | --- | --- | --- |
| Debug, Release, ASan, each | 152 | 1,672 | 152 |
| Without Runtime | 24 | 264 | 24 |
| Each four-profile phase | 480 | 5,280 | 480 |

All profiles retain 24 Context chains. Full profiles add eight simultaneous DNS
chains per version/mode/scheduler/layout combination, sharing immutable native
and Weave credentials. Both schedulers and both Windows I/O layouts run with
four workers. The test has no mandatory Runtime dependency; Linux's future
Runtime selection uses only its supported sharded layout.

Four source-frozen development entries pass before promotion. The root build
then passes **48 nonempty regression entries and two component-package entries**:
existing TLS engine, ALPN, metadata, credentials/provider, duplex/cancellation,
interop and PostgreSQL synthetic SNI/loader/native/direct-negotiation controls
remain green alongside the new adapter. Packages qualify isolated builds,
relocated consumers and self-contained headers, with no test/native dependencies
added to library-only consumers.

Twenty additional Windows ASan process repetitions pass consecutively without
retrying failures, retaining 20 full JSON audits: 33,440 connections, 3,040 actual
resumptions, 18,240 pre-handshake rejections and 9,120 verification rejections.
Combined with the two four-profile adapter phases, there are **44,000 audited
adapter connections and 4,000 actual resumptions**. The soak has one final JUnit
entry plus 20 process audits, not 20 fabricated distinct JUnit entries. There are
55 JUnit entries across the four development tests, permanent selection, packages
and soak. Each process drains children and confirms removal of its own certificate
directory. Windows ASan disables leak checking; native OpenSSL/libpq dependencies
are functional controls, not fully instrumented by the profile label.

The only retained failed command is the initial exploratory build: designated
initializers were out of TlsClientOptions declaration order. The corrected probe
passes all four development profiles before permanent promotion. Earlier shorter
exploratory passes are retained but excluded from the locked adapter totals.
No library, operational, sanitizer or qualification failure occurs in the locked
development/permanent/soak selections.

Ignored archive `tls-sni-streams-permanent-v1-20261010.zip` has SHA-256
`a2a1a27b6c8360ab57a57c3c069e91d001255baca06713b34c55c192190f2031`.
Its verification sidecar checks 548 source inputs, 33 passing command boundaries,
the retained failed build, exact nonempty inventories, complete peer audits,
archive/input digests and the original protected state. The pure verifier is
added after promotion, before the source-frozen soak; excluding that verifier
alone, permanent boundaries match the final source epoch. Functional permanent
test text matches the confirmed probe, and all library source hashes match the
preceding SNI checkpoint. Six primary scratch files are sealed then removed;
curated documentation is updated after sealing. HEAD, staged bytes and the
protected ASan DLL remain unchanged; no benchmark, staging, commit, push or upload.

This closes the Windows public resumed-stream SNI gate, not the pending Linux
adapter/ASan/package or real-server SNI controls. WSL remains unrepaired pending
approval. Broader deployment/security review, remaining connection/result
capabilities and final matched libpq measurements stay in scope. The callable
inventory is unchanged and is not a completion percentage or production certificate.

## Client Certificate Policy Development (2026-10-10)

`TlsHandshakeOptions::client_certificate` and PostgreSQL
`Options::client_certificate` now expose disable/allow/require. Keyword, URI,
service and explicit PGSSLCERTMODE loading map sslcertmode to the owning typed
setting, and OptionsInfo retains its nonsecret value. Default allow preserves
existing behavior. Nine library/header files change; two permanent option-schema
test expectations are migrated only after the development implementation passes.
No runtime, transport, exception or scheduling redesign is involved.

Disable clears identity only on the SSL object. PostgreSQL file-based loading
also clears ignored identity paths and owned passphrases/providers before the
credential factory, while preserving trust/revocation/version policy and the
original nonsecret requested configuration. Require observes native handshake
messages through an SSL-local OpenSSL callback with an Impl-stable argument;
it requires a request and an actual CertificateVerify proof, rather than the
presence of a configured certificate. The observer allocates/parses no TLS
messages, retains no buffers and runs inside serialized engine calls. Ordinary
allow/disable handshakes do not install it.

Required-mode session capture retains confirmed identity use. Changed modes
reject before consuming a one-shot session; matching actual resumption inherits
the original fact, not a fictitious fresh CertificateRequest. A fresh server
that declines the offer must request a usable identity in the full handshake
again. Missing proof is a sticky client_certificate_required failure before
the public adapter exposes application I/O or PostgreSQL sends Startup. This
does not guarantee the peer validates or authorizes the identity.

| Windows profile | Development entries | Permanent schema entries | PG Weave connections | Native libpq connections | Stream connections / actual resumes |
| --- | --- | --- | --- | --- | --- |
| Debug | 8 | 2 | 7,290 | 151 | 1,672 / 152 |
| Release | 8 | 2 | 7,290 | 151 | 1,672 / 152 |
| ASan | 8 | 2 | 7,290 | 151 | 1,672 / 152 |
| Without Runtime | 8 | 2 | 810 | 151 | 264 / 24 |

The exact nonempty JUnit inventories contain **40 qualified entries**. Every
development profile covers 80 ordinary PG peer scenarios, 24 pinned-OAuth
scenarios, three plaintext controls, 56 native TLS controls, three native
plaintext controls and 11 isolated service/environment-loader controls.
Public resumed-stream tests retain Context chains with DNS/IP verification
names, SNI enabled/disabled, required ALPN, verified native mTLS, lazy resumed
Task destruction, nonconsuming mode/name/ALPN/credential mismatches, one-shot
consumption, verification failures and authenticated shutdown. IPv4 TCP is the
transport even for IPv6 verification names; this is not IPv6 transport evidence.

Full profiles additionally exercise four-worker worker-affine/work-stealing
Runtime scheduling and sharded/shared IOCP with concurrent independently owned
sessions and immutable shared credentials. The reduced selection keeps both
Context and the blocking facade without linking Runtime. Native memory-BIO
engine controls cover both TLS versions, absent/provided/unsuitable identities,
server request/no-request, all three modes, stateful/ticket resumption, declined
offers and independent concurrent native handshakes. These are correctness
controls, not performance measurements or actual server-side query interruption.

The first retained failed CTest run is attributed to the exploratory probe:
it attempted output extraction after policy failure, but the engine intentionally
refuses output once poisoned. The corrected probe returns on failure and verifies
the sticky output error. A second retained failure is an actual implementation
gap: require was ignored on plaintext. An independent libpq 18.4 control rejects
that connection; the original Weave peer observed unexpected Startup bytes.
The implementation now rejects all non-TLS transports before Startup. That
unified guard includes local/GSS transport selection, but live local/GSS
certificate-policy deployment controls are not qualified by these TCP fixtures.
No failure is erased or selectively retried for favorable results.

Weave and independent native libpq clients use matching OpenSSL 3.6.5 headers and
runtime; libpq identifies as 18.4 and ICU as 78.3. The independent Python TLS peer
uses its bundled OpenSSL 3.0.16 only as a functional fixture, not a supported
production dependency. Windows ASan disables leak checking; native OpenSSL,
libpq, ICU and Python dependencies are not fully instrumented. A green Windows
ASan label does not qualify their internals or Linux leak behavior.

Ignored archive `tls-client-certificate-development-v1-20261010.zip` has SHA-256
`89007157521f6d3ea327adf11650337476c3dad49ac2fa11af4742bd7c62895c`.
Its manifest and sidecar independently validate 553 source inputs, every command's
input archive/source hashes and protected state, exact qualified inventories,
complete scenario sets, native versions, snapshot/disclosure counts, closure
and fixture cleanup. Earlier exploratory passing selections are retained but
excluded from the 40-entry totals. Curated documentation changes after sealing;
HEAD, staged bytes and the protected ASan DLL are unchanged. Primary exploratory
sources remain in the ignored active work directory for pending permanent
promotion, never staged or committed. No benchmark, commit, push or upload occurs.

Remaining work: permanent policy regression promotion, broader TLS/PG regression
and packaging, real-server mTLS/SCRAM-PLUS/reset/actual-query-cancellation,
local/GSS-specific controls, cancellation/adversarial soaks and Linux qualification.
WSL remains unrepaired pending approval after its storage failure. CRL directories,
native key integrations, remaining result/deployment gaps, independent review
and final matched libpq measurements stay in scope. The callable inventory
remains 128 Mapped, 11 Partial, three Open, 43 Different and eight External; this
is not a completion percentage or a production-readiness claim.

## Client Certificate Policy Regressions (2026-10-10)

The preceding confirmed Windows implementation is promoted without any library
changes. TLS owns client_certificates.cpp and client_certificates_streams.cpp;
the latter retains the required-mode ordinary/resumed stream and four-worker
controls. PostgreSQL owns its independent wire-peer client, parser/loader and
native libpq control. The previously mixed development probe is split at that
boundary: TLS tests never require PostgreSQL, and Runtime remains optional.
Native libpq is private and opt-in under WEAVE_POSTGRES_TLS_LIBPQ_TESTS; neither
installed consumers nor library-only builds acquire test dependencies.

Each focused permanent selection contains nine CTest entries. The broader
selection contains all 82 registered TLS/PostgreSQL entries in each profile;
81 have complete output in the original run, with the configuration entry
qualified separately after fixing its output capture.

| Windows profile | Focused policy | Broader qualified entries | Configuration confirmation |
| --- | --- | --- | --- |
| Debug | 9 | 81 | 1 |
| Release | 9 | 81 | 1 |
| ASan | 9 | 81 | 1 |
| Without Runtime | 9 | 81 | 1 |

Two Release component gates additionally pass isolated TLS/PostgreSQL builds,
relocated install consumers, standalone headers and missing-component checks.
The total is **366 qualified CTest entries**, including repeated policy controls,
not distinct workloads. All 370 recorded entries returned success, but four
original silent configuration entries are retained and explicitly unqualified.

The first independent seal rejected those empty outputs before creating an
archive. Windows CREATE_NO_WINDOW did not retain the configuration driver's
inherited child output. An ignored capture probe confirmed all five selections
on each profile before the final driver was updated. The permanent driver now
captures and prints stdout/stderr, preserves failing exits, and rejects empty
or inconsistent doctest case/assertion summaries. Final confirmations report
15 executed cases and 192 assertions per profile, with owned fixture cleanup.
This was an evidence-driver repair, not a production-library failure; the rejected
verifier and original entries remain archived rather than overwritten.

Both focused and broader runs independently audit the complete certificate
scenario matrix, reset/cancellation snapshots, pinned OAuth, actual native-peer
identity disclosure and required-mode resumed streams. Their full-profile runs
each observe 7,290 Weave PostgreSQL connections, 151 native connections and
1,672 public TLS connections with 152 resumptions; the without-Runtime runs
observe 810, 151 and 264 respectively, with 24 resumptions. These are repeated
functional controls, not SQL throughput, distinct deployments or actual query
interruption evidence. Engine checks, 11 loader controls, parser expectations
and complete peer transcripts are retained and independently validated.

Archive tls-client-certificate-permanent-v1-20261010.zip is stored outside the
checkout under E:/weave-work-cache/tls-cert-policy because of C: storage pressure.
SHA-256: 025ce330e4c86871c059a49d429e23c26a4b0e7fe7711f080ede72655bb85274.
Its manifest and sidecar verify 565 source inputs, 30 successful source-frozen
commands, full input archives, protected state, exact inventories and complete
audits. Build caches, project flags, registered tests, binary/runtime hashes,
package isolation and the separate capture-driver source epochs are recorded.
Seven permanent sources, two CMake registrations and the existing configuration
driver differ from the preceding checkpoint; production source is unchanged.
Primary development/promotion scratch sources are sealed then deleted. Only
curated documentation changes after sealing; HEAD, staged bytes and the protected
ASan DLL remain unchanged. No staging, commit, push, upload or benchmark occurs.

Weave/OpenSSL native controls use OpenSSL 3.6.5; libpq is 18.4. The Python peer's
OpenSSL 3.0.16 remains a functional test fixture, not a supported production
provider. Weave-linked ASan targets are instrumented; independent libpq control
executables and external libraries are functional baselines, not fully ASan
qualified. Windows leak checking is disabled.

Remaining gates include real-server certificate-policy/mTLS/SCRAM-PLUS/reset and
actual query cancellation, local/GSS-specific policy controls, sustained
cancellation/adversarial/lifetime soaks and Linux. WSL remains untouched pending
repair approval. CRL directories, native key integrations, result/deployment gaps,
wider review and final matched libpq measurements remain in scope. The callable
inventory stays 128 Mapped, 11 Partial, three Open, 43 Different and eight External;
this checkpoint does not establish complete parity or whole-stack production readiness.

## Windows Certificate Policy Server Controls (2026-10-10)

The new certificate selection policy now has actual PostgreSQL 18.6 Windows
development evidence, without restarting or modifying the unavailable WSL disk.
The portable EDB toolchain is pinned to the [vendor-published checksum](https://github.com/EnterpriseDB/edb-installers/issues/733),
its executables report 18.6, and complete extracted-file hashes are revalidated.
No service is installed and no existing PostgreSQL instance is modified.

Each full profile creates four disposable clusters: certificate-requesting and
non-requesting servers, separately restricted to TLS 1.2 and TLS 1.3. Both
SSLRequest and direct/required-ALPN negotiation exercise disable/allow/require
with file, prebuilt, absent and deliberately ignored invalid identity paths.
Actual pg_stat_ssl rows verify TLS and presented-client-identity facts; mandatory
channel binding exercises SCRAM-PLUS. Optional-certificate roles allow reset
policy changes; a separate verify-ca role confirms mTLS enforcement, including
server SQLSTATE 28000 rejection when the client deliberately sends no identity.

| Post-fix profile | Matrix cases | Session controls | Checks | Observed SQL cancellations |
| --- | ---: | ---: | ---: | ---: |
| Windows Debug | 80 | 488 | 18,034 | 900 |
| Windows Release | 80 | 488 | 18,034 | 900 |
| Windows ASan | 80 | 488 | 18,034 | 900 |

Each profile also contains eight separate strict-mTLS role controls, already
included in its session and check counts. Context and blocking clients run all
matrix cells. Four-worker clients exercise shared prebuilt credentials under
both schedulers and both IOCP layouts. Reset verifies changed identity selection
and a new backend PID; old cancel snapshots do not retarget the replacement.
Query cancellation waits for a server notice or observed active PgSleep backend,
asserts SQLSTATE 57014 and checks subsequent query reuse. Dispatch alone is not
counted as query interruption. All private clusters and fixtures are removed.

The initial ASan controls exposed a real Windows cancellation defect: a fully
written TLS CancelRequest was handled, the original SQL query reported 57014,
but the separate cancellation-response read reported system error 10054. Phase
diagnostics identified this read, not certificate setup or TLS send shutdown.
[Pinned libpq's cancellation implementation](https://github.com/postgres/postgres/blob/REL_18_4/src/interfaces/libpq/fe-cancel.c)
explicitly handles Windows' reset-as-closure behavior. Weave now accepts only
connection_reset at that final TLS cancellation read on Windows. It does not
accept arbitrary errors, failed writes, deadlines, cancellation, bad proofs or
response data, and does not relax ordinary TLS EOF validation.

An independent peer forcing TCP reset after validating the entire cancellation
key and client's TLS close_notify reproduces failure before the fix and passes
after it. The confirmed control is promoted into the existing cancellation suite:
112 cases each in Debug, Release and ASan, and 72 without Runtime. Four registered
CTest entries pass with full output and JUnit, including 40 forced-reset cases
covering 520 requests. The earlier ad hoc ASan matrix additionally validates 162
forced-reset requests. Existing malformed response, refusal, certificate,
deadline, token-cancellation, deferred lifetime and cleanup controls remain.

Immutable local evidence:
E:/weave-work-cache/pg-cert-server/pg-client-certificate-server-development-v1-20261010.zip.
SHA-256: 92384090a8fa329d07ac6a860aba71f485c9e5f14b1b7f4ab6b8ea0792aa0049.
The seal verifies 556 source inputs, 43 complete source-frozen commands, protected
state, input archives, raw outputs, JUnit, client binary hashes and native supplier
provenance. Ten failed commands remain explicitly failed: argument quoting,
private CMake imported-target scope, probe compilation, provider/header discovery,
Windows inherited pg_ctl output pipes, the original/repeated ASan cancellation
failures and the deterministic before-fix witness. The loader inspection is a
diagnostic with a failing child, not a successful client gate. The first seal
attempt rejected a helper's wrong provenance-field name before archive creation;
the corrected verifier checks the actual supplier schema and full file hashes.

One production source and two existing permanent test sources change. No public
API or TLS engine changes. New real-server sources remain ignored development
inputs pending independent native controls and final promotion; none are staged
or committed. Debug/Release pre-fix successes are retained separately, not counted
as current-source evidence. The external PostgreSQL server and its dependencies
are not ASan-instrumented, and Windows leak detection is disabled. These are
correctness controls, not benchmarks, a security audit or production certification.

Remaining work includes native libpq real-server controls, final real-server
regression promotion, broader post-fix regressions, local/GSS-specific require
policy, sustained adversarial/cooperative-cancellation/lifetime soaks and Linux.
CRL directories, key integrations, result/deployment gaps and final matched libpq
measurements remain in the full objective. HEAD, staged bytes and the protected
ASan DLL remain unchanged; WSL remains untouched.

The subsequent [post-fix qualification](#windows-cancellation-post-fix-qualification-2026-10-10)
closes the broader Windows regression/package item above.

## Windows Cancellation Post-Fix Qualification (2026-10-10)

All registered TLS/PostgreSQL targets are rebuilt after the cancellation fix.
The complete selection passes without test retries on identical first-party
bytes; Release additionally passes isolated, relocated TLS and PostgreSQL package
consumers. Library-only package builds fetch no comparison/test dependencies.

| Profile | TLS/PostgreSQL entries | Cancellation cases | Forced-reset cases |
| --- | ---: | ---: | ---: |
| Windows Debug | 82 | 112 | 12 |
| Windows Release | 82 | 112 | 12 |
| Windows ASan | 82 | 112 | 12 |
| Windows without Runtime | 82 | 72 | 4 |

The total is 330 CTest entries including two package entries, not 330 individual
assertions. Cancellation contributes 408 scenario executions, including 40
forced-reset scenarios validating 520 requests. These synthetic request counts
are not SQL interruption counts. The preceding real-server archive supplies
2,700 actual SQL cancellations on the same production implementation; those
are not rerun or counted as new cancellations by this gate.

Each registered inventory is matched against CTest's current configuration and
the previously qualified 82-name inventory. Complete certificate-policy peer
matrices, native version guards, public ordinary/resumed streams and fixture
cleanup match the preceding policy qualification. Every configuration driver
positively validates its five selections and 192 assertions. ASan project flags,
optional-Runtime variants, native dependency separation and binary hashes are
checked; native controls and external libraries are not fully instrumented.

CTest's default JUnit payload cap truncates 40 entries each in Debug, Release
and ASan, and 37 without Runtime. The original XML is preserved and used for
terminal inventories only. Full verbose logs contain no truncation markers.
The auditor extracts complete per-test output after CTest's metadata preamble,
checks untruncated XML equality or truncated-prefix agreement, and audits the
complete positive matrices from those logs. No assertion evidence is fabricated
by expanding or replacing the original XML.

Immutable local evidence:
E:/weave-work-cache/pg-cert-server/pg-cancel-postfix-regressions-v1-20261010.zip.
SHA-256: f73d68f34bf8f85cd64514070e0de658f6a0aa60a9cb44960983b37ed62f86cb.
The archive includes 557 source inputs, nine source-frozen commands, original
JUnit, complete logs, parsed output, build/package metadata and binary hashes.
Two rejected auditor versions are retained: one searched for a removed CTest
prefix when identifying cancellation modes; the other included CTest's ASan
environment preamble in child output. Both failed before archive creation.
The corrected auditor independently checks the full inventories and matrices.

No production, public API, CMake or permanent test code changes in this gate.
The already sealed ad hoc reset driver and this gate's primary auditor source
are removed from the repository after archival; a verified auditor copy remains
outside it. Real-server development inputs remain pending native comparison and
permanent promotion. Curated documentation changes are checked separately from
the tested source boundary. HEAD, staged bytes and the protected ASan DLL stay
unchanged; WSL is not touched.

This closes broader Windows post-fix regressions and packaging, not Linux,
native real-server policy controls, local/GSS-specific policy or sustained
adversarial/cooperative-cancellation/lifetime soaks. Windows leak checking is
disabled. No performance measurement, security audit or production certification
is claimed. The remaining feature/deployment gaps and final matched libpq
measurements in the full objective are unchanged.
