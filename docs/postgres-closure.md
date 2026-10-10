# TLS And PostgreSQL Checkpoint (2026-10-10)

This closes the fixed implementation batch, not every possible deployment or
literal libpq ABI. No additional protocol or runtime redesign is included.

## Implemented

- TLS client/server PEM, DER and configured OpenSSL STORE private keys, including
  encrypted-key callbacks, strict matching and owned secret/locator cleanup.
- Trusted hashed CRL directories, alongside files, with leaf/chain verification.
- Permanent client-certificate/SNI controls, public resumed streams and native
  Windows PostgreSQL/libpq startup/reset/actual-query-cancellation fixtures.
- Checked INSERT-OID accessors, bounded owning table/delimited/HTML display,
  and checked application-created diagnostic Outcomes.
- Native Windows plain/TLS serial and 32-connection 1/2/4-worker comparisons
  against libpq, plus nine retained-result memory shapes.

[TLS setup](tls.md), [PostgreSQL setup](postgres-connections.md),
[results](postgres-results.md), [serial/memory measurements](postgres-benchmarks.md),
[concurrent measurements](postgres-concurrency.md).

## Windows Verification

MSVC 19.44; Weave/native controls use OpenSSL 3.6.5, libpq 18.4 and owned
PostgreSQL 18.6 fixtures. Runtime controls use four workers, both schedulers and
both supported IOCP layouts. The user's Windows PostgreSQL service is untouched.

| Selected Gate | Result |
| --- | --- |
| Final feature Debug | 15/15 passed |
| Final feature Release, including TLS/PostgreSQL packages | 17/17 passed |
| Initial ASan | 14/15 passed; cancellation race reproduced and fixed |
| Post-fix Debug: units, synthetic cancellation, real certificate server | 3/3 passed |
| Post-fix Release: same plus generic real-result server | 4/4 passed |
| Post-fix ASan: units, synthetic cancellation, real certificate server | 3/3 passed |
| Final platform-aware certificate fixtures and synthetic benchmark tooling | 3/3 passed |

The failure is not hidden as a passing initial ASan run. A delayed control
reproduced Windows connection reset during TLS `shutdown_send`, after the entire
cancellation packet had been sent. PostgreSQL may close that one-way connection
before receiving TLS close_notify. Only that post-write Windows reset is accepted;
handshake/write errors, unexpected response data, cancellation and deadlines
remain errors. Ordinary TLS EOF semantics are unchanged. The identical delayed
fixture passed with the fix, then all instrumentation/delays were removed.
Permanent early-reset controls cover Task/blocking cancellation and runtime
scheduler/layout combinations, on protocols 3.0 and 3.2.

Each Weave real-certificate gate covers 80 policy cases, 488 sessions, 900 actual
SQL cancellations and 448 stale-key controls across four owned clusters. Native
libpq independently covers 56 cases, 144 sessions and 192 SQL cancellations.
Owned clusters and credential fixtures are removed after runs. ASan on Windows
uses `detect_leaks=0`; dependencies are not fully sanitizer-instrumented.

The generic server controller also uses file-backed pg_ctl output, avoiding
inherited pipe handles that otherwise prevent EOF while the server is running.
Two relocated component-package tests cover standalone headers and isolated
TLS/PostgreSQL consumers. Harness-only configure/build mistakes and all failed
investigation rounds remain in local evidence; none count as successful tests.

## Linux Verification

Ubuntu's filesystem became read-only after the host drive filled. With explicit
approval, WSL was shut down and its 64,691,896,320-byte VHD cold-copied to E:.
Source/backup SHA-256 matched:
`c9221256d1b92e60fc66213d40bb69e2d7144658510ce89fff5f0ac5a65b791d`.
Windows canceled the requested elevated offline mount; no offline e2fsck ran.
A normal restart replayed the ext4 journal: the kernel reported recovery complete,
root mounted read/write, and temporary-file creation/sync/removal passed. The
backup remains preserved and the Windows PostgreSQL service remains running.

Fresh GCC 15.2 / CMake 4.2 / Python 3.14.4 profiles use io_uring, liburing 2.14,
OpenSSL 3.5.5, ICU 78.2 and PostgreSQL/libpq 18.6. These are tests of current code,
not inherited Windows or earlier Linux results.

| Selected Linux Profile | Result |
| --- | --- |
| Debug: Runtime, GSS, LDAP and native libpq controls | 28/28 passed |
| Release: same selection | 28/28 passed |
| Debug ASan: same selection, leak detection enabled | 28/28 passed |
| Debug without Runtime/LDAP or native comparison controls | 24/24 passed |
| Debug without Runtime/GSS/LDAP or native comparison controls | 24/24 passed |
| Release TLS/PostgreSQL relocated packages | 2/2 passed |

The **134 passing test/package entries** include public SNI/resumption streams,
client-certificate selection, CRL/DER/STORE keys, credentials/option schema,
result/OID/display/failure units, cancellation and owned real mTLS/query-cancellation
fixtures. Runtime profiles use four workers, both schedulers and sharded io_uring;
Linux does not support shared I/O. The Windows-specific real certificate-policy
matrix remains Windows-specific; Linux's real-server checks use the existing
portable mTLS/direct-TLS fixtures, not a claimed port of that matrix.

An initial Linux attempt passed 24/28 entries. Two confirmed fixture bugs caused
the four failures: expected sessions assumed two Windows I/O layouts on Linux,
and native libpq rejected a generated client's 0644 key. The runner now counts
Linux's single layout, and the native fixture uses 0600. The unmodified-production
corrected selection passes all five profiles; the initial failure is retained.
Missing optional LDAP development headers were installed after a separately
recorded configure refusal. Qualification builds use persistent owned cache paths
because reboot discards this system's /tmp. None of these fixes relax TLS policy.

## Measurement And Compatibility Boundaries

No performance optimization was attempted in this batch. Weave is not uniformly
faster than libpq: the native plaintext serial batching/row workloads are slower,
and larger result graphs retain more backing storage. Noise is reported per metric;
Windows process CPU time is unavailable, not zero. The four-worker cells complete
an initial runner's double-core-reservation omission without replacing its existing
one/two-worker samples. Exact working-tree source inputs and all raw measurements
are [published separately](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261010-postgres-final),
not tracked Git artifacts. Asset SHA-256:
`051b66ba58b15520bcb19b3c622c740bbfa4d7b843b5813cad876225b929c77c`.

The [193-function audit](postgres-api-audit.md) records 130 Mapped, eight Partial,
zero Open, 47 Different and eight External interfaces. These are honest model
differences, not a claim that every libpq deployment is reproduced. There is no
legacy ENGINE installation, native-handle export, pager subprocess, arbitrary
wire-error ResultSet or async-signal-safe cancellation API. STORE uses configured
providers; HSM/FIPS/provider deployments, Windows domain interoperability and
independent security review are not qualified by these local fixtures. Existing
OAuth/LDAP/replica guides retain their explicit deployment limits.

HEAD, the existing staged bytes and the protected ASan runtime DLL are unchanged.
No source commit or push is part of this checkpoint. Earlier detailed records
remain [historical evidence](postgres-qualification.md), not a new work queue.

## Evidence Archive

Local `weave-tls-postgres-closure-20261010.zip` retains 63 command records,
21 independently hash-checked source snapshots, JUnit inventories and the failed
investigation rounds. All archived entries were rehashed after creation. SHA-256:
`0a97085d8f86e4195f96a59d2e1dafc1fc0c76dd032f0fd2672fa3d5ed5128be`.
Temporary probe sources/controllers were archived, then deleted; raw evidence
and the verified Ubuntu VHD backup remain outside tracked source.
