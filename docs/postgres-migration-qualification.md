# PostgreSQL Migration Qualification

This closes the fixed follow-on batch: six TLS modes, explicit libpq-style
credential loading, connection-phase timings and sensitive key logging. It is
not drop-in libpq behavior, a security audit or domain/hardware certification.
[Contracts](postgres-connections.md), [diagnostics](postgres-diagnostics.md)
and [key logging](tls.md#sensitive-key-logging).

## Validation On 10 October 2026

Windows used MSVC 19.44, OpenSSL 3.6.5, libpq 18.4 and disposable PostgreSQL 18.6.
WSL Ubuntu used GCC 15.2, OpenSSL 3.5.5 and PostgreSQL/libpq 18.6. No user's
database service was stopped, reconfigured or used as a fixture.

| Gate | Windows | WSL Linux |
| --- | --- | --- |
| TLS verification/metadata and TLS 1.2/1.3 key logs | Debug, Release, ASan passed | Debug, Release, ASan passed |
| Twenty policy scenarios on both TLS versions | 720 sessions per profile passed | 400 sessions per profile passed |
| Independent libpq six-mode controls | Debug, Release, ASan passed | Debug, Release, ASan passed |
| Options, explicit loading, schema and report regressions | Debug, Release, ASan passed | Debug, Release, ASan passed |
| Owned real-server mode/query/cancellation/key-log matrix | 162 sessions / 3,389 checks per profile passed | 90 sessions / 1,891 checks per profile passed |
| Existing cancellation, configuration snapshots, direct TLS and pinned reconnect | Debug passed; post-fix cancellation also Release/ASan | Debug passed |
| Isolated installed TLS/PostgreSQL consumers and header probes | Debug passed | Debug passed |
| PostgreSQL without Runtime | Isolated PostgreSQL package passed | Reduced Debug four-suite gate passed |
| Local SSPI provider/startup controls | Debug passed | Not applicable |

Runtime matrices use Context, the same-engine blocking facade, four-worker
worker-affine/work-stealing runtimes, and both Windows IOCP layouts. Linux uses
its supported sharded layout. The independent native control does not link Weave.
It checks thirteen cases per TLS version, not Weave-specific security restrictions.

Permanent wire peers audit Startup, query, Terminate and exact cancellation
packets. Cases include valid negotiation refusal, broken-handshake pinned retry,
plaintext-to-TLS upgrade, malformed negotiation, authentication rejection,
verification rejection, forbidden cleartext-password transmission and timeout.
Cancellation retains the actual transport after upgrade/fallback. Reports check
final errors, phase durations and same-endpoint retry history.

Key-log tests cover concurrent shared/independent contexts, client/server native
TLS 1.2/1.3 record structure, private-file reopening, invalid paths and Linux
unsafe permissions. PostgreSQL checks file output, enabled-only snapshots and
removal after credential/stream owners drain. Loader tests cover explicit opt-in,
ordinary secure defaults, precedence, legacy SSL policy, conventional credential
paths, overrides and Linux key permissions. Live tests use explicit fixture
credentials, not the user's home; not every native keyword/default is promised.

## Attributed Corrections

Windows PostgreSQL can reset a plaintext cancellation socket after a complete
CancelRequest. This now has the existing TLS path's dispatch-only meaning. The
exception is Windows-specific and follows a successful full send; ordinary
queries and incomplete sends retain errors. Permanent reset-before/after-half-close
peers cover both protocol versions, blocking dispatch and runtime layouts/schedulers.
Real-server tests pass. Dispatch still does not guarantee SQL interruption.

An installed consumer expected the old `validation:` text; it now checks phase
timings and the new profile API. Initial key-log cleanup failures came from a
still-open Windows test reader, not retained TLS owners. Native hostname rejection
can close/reset after the handshake; the peer accepts that only in its expected
negative case. Failed development runs are not counted as passing gates.
Mode-only typed configuration and the legacy plaintext flag now produce
consistent TLS-mode/verification snapshots; permanent option tests cover both.

## Deployment Boundaries

Windows is not domain-joined (`WORKGROUP`); WSL OpenSSL lists only the default
provider. No domain credentials, HSM, PKCS#11 provider or FIPS deployment was
available. No such positive qualification is claimed. Configured OpenSSL STORE
support remains usable; file-STORE tests are not hardware evidence. Windows
enterprise certificate-chain policy is distinct from ROOT import.

Weak modes and key logging are not ordinary defaults. Native fallback, local
sockets, deadlines, provider setup and password restrictions retain documented
Weave differences. Third-party libraries are not fully instrumented; Windows
ASan is not leak evidence.

Ordinary tests register `weave_tls_verification` and `weave_postgres_tls_modes`.
`WEAVE_POSTGRES_TLS_LIBPQ_TESTS` adds the native control; `WEAVE_POSTGRES_SERVER_TESTS`
and `WEAVE_POSTGRES_SERVER_BIN` enable owned live qualification. These are
functional tests, not benchmarks. No benchmarks or performance claims were
updated. Temporary proof sources were deleted, existing staging/HEAD preserved,
and the previous closure archive and WSL recovery backup left intact.

## Hosted Windows Closure (11 October 2026)

The first hosted checkpoint run is retained as
[failed evidence](https://github.com/c-schembri/weave/actions/runs/38052888754),
not counted as passing qualification. Debug failed four of 94 checks; Release
failed five. ASan exceeded the whole-job limit with an incomplete inventory.

New Windows key-log files now explicitly assign ownership to the effective
user rather than inheriting an elevated token's Administrators-group owner.
The private user/SYSTEM ACL and existing-file rejection policy are unchanged.
The verification test checks actual file ownership and guards failed setup
Results instead of crashing after exception-free REQUIRE failures.

The COPY-reset peer now permits a successful queued send before observing the
reset during COPY completion; both paths must retain the terminal failure.
The TLS-refusal report fixture prepares its verified credential snapshot before
opening connections, rather than repeatedly loading platform trust on workers.
No protocol error, peer timeout or runtime matrix is silently ignored.

Package compiler commands have separate build budgets; operational test deadlines
are unchanged. CI saves successfully built dependencies before tests, so later
failures no longer discard the expensive cold build. Its 60-minute ceiling covers
cold dependency compilation, examples and the full correctness/package suite;
it is not a benchmark deadline. Benchmarks remain disabled.

Local post-correction Debug passed the four affected checks, Release passed both
key-log/mode checks and COPY-reset controls, and ASan passed verification, TLS
modes, COPY/failure and connection-report controls. Hosted rerun results must be
read from the Windows workflow, not inferred from those local passes.
