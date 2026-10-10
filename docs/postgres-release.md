# PostgreSQL Checkpoint

The TLS/PostgreSQL implementation batch is closed. This freezes the documented
scope; it does not certify production readiness or promise drop-in libpq behavior.
New features, including connection pooling, belong to separate work.

## Supported Contracts

- Exception-free `Task<T>` operations and a same-engine blocking facade.
- Queries, prepared statements, batching/pipelines, streaming rows and portals,
  COPY, notifications, replication messages and large objects.
- Verified TLS by default, explicit weaker compatibility modes, mTLS, revocation,
  sessions and private traffic-key logging. Security downgrades are bounded by
  the documented policy, not unrestricted native fallback.
- Explicit configuration loading, multi-host connection policy, cancellation,
  reset, diagnostics and owning results/callbacks.
- Windows IOCP and Linux io_uring, with optional modules and installed packages.

[Main API](postgres.md), [exact parity and differences](postgres-parity.md),
[connection policy](postgres-connections.md), [TLS policy](tls.md).
The guides define lifetime, concurrency and cancellation requirements; the list
above is not a substitute for those contracts.

## Qualification

[Original closure](postgres-closure.md) and
[migration follow-on qualification](postgres-migration-qualification.md) record
the completed local Windows/WSL Debug, Release, sanitizer, native libpq,
real-server and packaging gates. Windows CI runs correctness tests and examples
in Debug, Release and AddressSanitizer, without benchmarks. Optional external
fixtures are not implied to run in ordinary hosted CI.

Domain-joined Windows authentication, hardware/FIPS providers and application
deployment policy require separate environment-specific qualification. There has
been no independent security audit. Windows ASan is not leak evidence, and
third-party dependencies are not fully instrumented. Linux correctness CI and
cross-platform performance measurements remain separate follow-up work.

Performance comparisons are workload-specific, not a universal win over libpq.
[Measurements and limitations](postgres-benchmarks.md).

## Scope Freeze

Fix reproducible defects in these contracts without silently expanding the API.
Do not treat optional deployment qualification or deliberate libpq model
differences as an invitation to keep adding features to this batch.
The project remains experimental and has no selected project license.
