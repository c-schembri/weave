# Testing And Deployment Boundaries

Correctness and performance are separate. Windows CI runs Debug, Release and
AddressSanitizer with TLS/PostgreSQL/MySQL enabled, without benchmark execution or
Tokio/libpq benchmark dependencies. Artifacts retain JUnit and failed-test logs.
Sanitizers instrument first-party code, not every external provider.
JSON checks cover Glaze struct mapping, strict document consumption, diagnostics
and fixed-buffer bounds. OAuth additionally tests decoded-key ambiguity, resource
budgets, integer token semantics and cleansing on success/failure.

## Run Correctness Tests

```sh
cmake --preset windows-postgres
cmake --build --preset postgres-debug --parallel 4
ctest --preset postgres-debug --label-exclude benchmark --no-tests=error

cmake --preset asan-postgres
cmake --build --preset asan-postgres --parallel 4
ctest --preset asan-postgres --label-exclude benchmark --no-tests=error
```

Supply the required OpenSSL/ICU package paths when not discoverable.
[Build guide](build.md). Verify that an intended selector runs a nonempty inventory;
do not report empty/skipped/missing tests as evidence. Run timing-sensitive fixtures
without competing builds or another full suite on the same machine.

The tests cover coroutine/lease lifetime guards, native cancellation draining,
timer races, bounded synchronization, module packaging/standalone installed headers,
TCP/local endpoints and fragmentation/refusal/reset/half-close. Runtime controls
exercise both schedulers and four workers; Windows also covers both IOCP layouts.
Linux tests use actual io_uring, not an emulation or inherited Windows result.

GCC 15.2's Release ASan build does not tail-transfer the 20,000-level coroutine
construction test: it exhausts an 8 MiB stack, while the same test passes with
`ulimit -s 65536`. Linux sanitizer qualification currently uses that larger stack.
This verifies lifetime cleanup, not bounded-stack construction in that instrumented
build; retain the 8 MiB failure rather than claiming a clean sanitizer gate there.

## TLS And PostgreSQL

Synthetic independent peers verify framing, SQL recovery, malformed/oversized packets,
COPY/pipeline/replication directionality, cancellation, diagnostics, callbacks,
metadata and bounded result storage. TLS checks include TLS 1.2/1.3, verification,
SNI/ALPN, mTLS, shutdown/truncation, resumption, CRLs/OCSP and secret/provider handling.

Optional `WEAVE_POSTGRES_SERVER_TESTS=ON` uses disposable primary/standby clusters,
ephemeral ports and generated private credentials. The runner starts/stops/removes
only its own clusters; it must not alter an existing PostgreSQL service.
`WEAVE_POSTGRES_RESULT_LIBPQ_TESTS`, `WEAVE_POSTGRES_TLS_LIBPQ_TESTS`,
`WEAVE_POSTGRES_PASSWORD_LIBPQ_TESTS` and `WEAVE_POSTGRES_DESCRIPTION_LIBPQ_TESTS`
add private native-reference controls, not library dependencies.
`WEAVE_POSTGRES_KERBEROS_TESTS=ON` requires its explicit Linux tooling.

Pool regressions cover capacity/reuse, concurrent Context/Runtime borrowers, per-worker
affine pools, rollback/DISCARD ALL, timeouts/cancellation, closure and retirement.
Live checks verify actual server-session cleanup over plain and verified mTLS/SCRAM-PLUS.

Migration description/history and CMake-generation checks run in the default suite.
With server tests enabled, `weave_postgres_migrations_live` verifies atomic apply/down,
concurrent runners, dirty history and cancellation cleanup on disposable databases.

Borrowed-input/COPY-buffer regressions cover wire parity, NULL/binary parameters,
partial/empty messages, malformed/oversized framing, cancellation, SQL recovery,
generation/lifetime guards and bounded whole-message traces. Optional live checks
exercise Context, four-worker runtimes and BlockingConnection over plaintext/mTLS.

Before this pooling change, the fixed TLS/PG closure ran Windows Debug/Release/ASan
and five Linux full/reduced profiles with relocated consumers. Keep that scope
distinct from tests of later work. [Historical frozen checkpoint](https://github.com/c-schembri/weave/blob/4f24d614b31708b10007072b2b9e6aa323249bad/docs/postgres-closure.md)
and [detailed qualification history](https://github.com/c-schembri/weave/blob/4f24d614b31708b10007072b2b9e6aa323249bad/docs/postgres-qualification.md)
retain failures, exact inventories and archive hashes. They are evidence, not an
expanding implementation backlog or certification of future revisions.

## Deployment Limits

Weave is experimental. Passing tests is not an independent security audit, every
libpq deployment, arbitrary OpenSSL provider or literal C ABI compatibility.
The [193-function mapping](postgres-api-audit.md) records concrete differences.

- Keep OpenSSL, ICU, liburing and system security providers patched. Tests do not
  qualify every version, custom provider, HSM, FIPS policy or enterprise trust store.
- Windows ROOT import is not the full native chain/revocation policy. Configure
  trusted CAs/revocation and refresh immutable credentials/staples deliberately.
- Validate Windows domain/Kerberos SPNs, service accounts, delegation and tokens
  in the target environment; synthetic/local tests are not enterprise interoperability.
- OAuth issuer/token trust and caching belong to the application. Device flow is
  bounded HTTPS, not a general HTTP/OIDC/JWT/refresh-token stack.
- LDAP service lookup is opt-in anonymous plaintext; use a trusted network or an
  external authenticated configuration system. It is not LDAPS/StartTLS support.
- Replication is raw transport. Durable WAL, feedback, timeline following, archive
  restoration, decoding and complete replica workflows remain application work.
- Logical protocol limits do not cap total process memory. Bound accepted clients,
  application-retained results and callback/output queues separately.
- No legacy ENGINE installation, native-handle export, pager subprocess or
  async-signal-safe cancellation API is supplied.

Performance is manual and hardware/workload-specific; no universal win or hosted
CI capacity claim follows from a correctness pass. [Benchmark methodology](benchmarks.md).
