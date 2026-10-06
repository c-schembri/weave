# Development priorities

Preserve the small, exception-free API, explicit lifetime model and optional
module boundaries. Implement these stages in order rather than building all
features at once. Performance measurements are manual and only needed for
intentional performance work; correctness CI never runs benchmarks.

## 1. Correctness hardening

The first pass adds a single-listener, 1024-client warmup/disconnect regression
under both IOCP layouts and both successful-completion modes. Debug, Release and
AddressSanitizer are the validation configurations. Benchmark failure diagnostics
must identify setup versus measured-I/O failures and preserve incomplete evidence.

Still open: the 2026-10-06 sharded/shared diagnostic's setup/warmup failure. Its
original logs are insufficient to identify the cause. Passing later runs or new
regression tests does not establish a fix. Use the new diagnostics to narrow a
recurrence, then add a targeted reproducer before changing runtime behavior.

The follow-up investigation confirmed and fixed a separate Windows backlog bug:
plain `listen(..., 8192)` admitted only 200 queued connections on this host.
Explicit larger backlogs now use `SOMAXCONN_HINT`, and the Asio/Tokio comparison
servers request the same native hint. A no-accept regression fails with the old
implementation and passes with the fix. See [the investigation](tcp-backlog.md).
This is not proof that either historical timeout had the same cause.

The new 1024-client test also timed out once in shared-IOCP, queued-success mode
with 948 clients warmed while Debug, Release and sanitizer suites were running
concurrently on the same host. The sanitizer integration suite subsequently
passed in isolation and in three consecutive repetitions, without increasing
the test's deadline. This is not proof that the two failures share a cause or
that connection setup under host-wide pressure is reliable.

Further hardening should cover admission/shutdown races, cancellation and
completion races, connection resets, partial I/O, and buffer/frame destruction.
Do not change scheduling defaults based on a single benchmark host.

The reproducible extreme-pressure cleanup failure was traced to the common
external load generator's bulk socket closes, not a stalled Weave coroutine.
Socket closure now has an explicit `CLOSED` acknowledgment and a bounded
25-second phase, followed by the original five-second process-exit limit. The
total manual job deadline remains five minutes. See the backlog investigation
for the failing/progress evidence and the complete post-fix cleanup runs.

## 2. DNS and IPv6

Implemented on Windows: owned IP addresses/endpoints, IPv6-only and explicit
dual-stack listeners, asynchronous hostname resolution, and sequential
multi-endpoint connection fallback. Numeric operations bypass DNS. Resolver
cancellation drains native completion before releasing query storage, and
public headers contain no Windows types. See [addresses and DNS](addresses.md).
Happy Eyeballs racing and a Weave-owned DNS cache remain outside this first pass.

Validation: Debug and Release passed 16/16 correctness checks, including isolated
component builds, relocated consumers, and standalone installed headers. The
final sanitizer run passed 15/15 with packaging excluded. DNS tests use localhost
or a per-query injected native provider, not external DNS. The multicore test
opens 512 IPv6/DNS clients across four roots for every scheduler/layout/completion
combination. Temporary probes were deleted; performance comparisons were not rerun.

Overlapping local build/test runs also produced two echo-example timeouts with
previously empty diagnostics and one five-second backlog-test timeout. Isolated
and final suite reruns passed without increasing those limits. Failure evidence
is retained under ignored `benchmarks/results/network-validation-20261006-152458/`;
echo failures now include process metadata and Python tracebacks. These observations
are not proof that host-pressure timeouts or the historical warmup failure are fixed.

## 3. Channels and semaphores

Implemented: bounded move-only channels with backpressure and drain-on-close,
and FIFO semaphore acquisitions with owned RAII permits. Pending waits observe
task cancellation and Context stop, and wake on their captured executor.
Tests cover closure, ownership, cancellation races, independent Context threads,
and both schedulers/layouts with four workers. [Contracts](synchronization.md).

## 4. Composable streams and TLS

Implemented: Core stream concepts/helpers and optional OpenSSL 3 TLS adapters.
TLS supports verified client/server handshakes, TLS 1.2/1.3, ALPN, full-duplex I/O,
and explicit graceful shutdown. TLS dependencies do not enter TCP-only consumers.
[Stream contracts](streams.md) / [TLS contracts and limitations](tls.md).

This is an initial experimental implementation, not a security audit or a complete
production TLS stack. Revocation integration, mTLS, session-cache controls, and
long-running hostile-peer stress remain future hardening work.

## 5. Linux io_uring

Implemented initial sharded-ring Contexts, TCP, DNS, timers, cancellation,
synchronization and TLS, using the existing runtime schedulers and public API.
Linux Debug, Release and ASan validation are available through CMake presets.
[Linux requirements and limitations](linux.md).

Shared-ring collectors, Linux correctness CI and platform-matched measurements
remain follow-up work before cross-platform performance claims. No epoll fallback
or macOS backend is planned.
