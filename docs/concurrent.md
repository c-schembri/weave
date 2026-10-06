# Concurrent Networking Validation

The adoption goal is better ergonomics without a meaningful networking
regression. A win in TCP throughput is not required, and a tiny coroutine-chain
regression alone is not a rejection criterion. Task is now the single coroutine
model; historical artifacts retain the evidence from evaluating its prototype.

## Workloads

`weave_concurrent` measures native Tasks with explicit-result handling, native
Tasks with automatic propagation, and the pinned Asio baseline:

| Connections | Client workers | Payload | Client CPU work per response |
|---:|---:|---:|---:|
| 64 | 4 | 1 KiB | Payload validation |
| 1024 | 1 | 1 KiB | Payload validation |
| 1024 | 4 | 1 KiB | Payload validation |
| 1024 | 8 | 1 KiB | Payload validation |
| 256 | 4 | 64 KiB | Payload validation |
| 1024 | 4 | 1 KiB | Validation plus 512 integer-work steps |

Both Weave scheduler modes run every workload. Asio runs both sharded io_contexts
and a shared io_context; those are not claimed to be identical scheduling
algorithms to Weave's. The sharded baseline is displayed alongside worker-affine
Weave, and the shared baseline alongside work-stealing Weave. Both Asio variants
are retained in the raw data.

All six implementations use persistent connections, TCP_NODELAY, identical
payloads, full payload verification on every exchange, and one long-lived root
per connection. The explicit reference uses `as_result` at each operation and
manually checks errors; the candidate uses bare fallible awaits. Both use the
same native Task socket operations and runtime roots. The former coroutine
implementation and prototype runtime adapters have been deleted. Historical
version comparisons require a baseline artifact, not these two current functions.

## Measurement

The legacy Google Benchmark research mode described below starts a fresh
four-worker echo peer for each repetition in a **separate process**. The newer
[paired CI mode](gate.md) instead reuses the runtime, peer, sockets, and buffers
across baseline, candidate, and same-code control windows. Both modes use the
same exchange and measurement routines.
The peer uses the existing `AsyncEchoPeer` implementation. The parent sends no
protocol messages in the measured window. A job object contains the child process
so a terminated test runner cannot leave it running indefinitely.

All connections perform four verified warmup exchanges. A readiness barrier
prevents measurement from starting before all warmups finish. Native events wake
the measuring thread at readiness/completion; no polling sleep is added to the
measured completion latency. Each root issues sequential write-all/read-exactly
exchanges until the shared deadline, then finishes its in-flight exchange. Wall
time ends after every root is joined. Setup, peer startup/shutdown, socket cleanup,
and percentile sorting are excluded from this manual measurement window.

- Throughput counts completed, validated roundtrips over the full measured
  interval, including final draining.
- Latency starts before write-all and ends after read-exactly, payload validation,
  and optional CPU work. Every measured roundtrip is recorded in preallocated,
  connection-local storage. There is no sampling or hot-path histogram lock.
- p50/p95/p99/p99.9 use exact sorted samples with nearest-rank quantiles. Reported
  medians across repetitions are medians of those per-repetition percentiles,
  not a pooled percentile. Minimum/maximum per-connection progress is recorded.
- Client CPU includes all benchmark-process threads, including runtime and join
  overhead, but excludes the peer. `GetProcessTimes` supplies CPU seconds and
  core equivalents; `QueryProcessCycleTime` separately supplies cycles per
  completed operation. The peer's counters are also recorded independently.

The Windows APIs sum process-thread CPU usage, including user and kernel work.
CPU times use 100 ns units, which must not be mistaken for effective measurement
resolution; the short smoke tests exhibited substantial quantization. Cycle
counts are retained separately and are **not converted to CPU seconds**.
[GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes),
[QueryProcessCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryprocesscycletime)

These are closed-loop, fixed-concurrency results. Slow clients generate fewer
requests; this is not a fixed-offered-rate or coordinated-omission-corrected
service-latency test. Loopback, a shared host, and a four-worker peer may mask
some client differences. CPU measurements help detect costs hidden by peer
throughput, but these tests do not establish arbitrary production SLOs.

## Reproduction and interpretation

Build Debug and Release using the instructions in [README.md](../README.md), then:

```sh
ctest --preset debug
ctest --preset release
python scripts/bench_concurrent.py --duration-ms 1000 --repetitions 7
```

The script runs two independent, randomly interleaved sets. The 36 cases produce
252 raw repetitions per set. Profiling and ASan must be off for timing. CPU
frequency/affinity are not forced; do not run builds or tests alongside the
benchmark. Source and executable hashes, compiler, OS, hardware, dirty-worktree
state, individual repetitions, and aggregates are preserved.

Provisional limits, selected before the repeated comparison:

- No more than 5% throughput regression.
- No more than 5% increase in client cycles per completed roundtrip.
- No more than 10% increase in p99 latency.

CPU seconds per operation and core equivalents remain visible diagnostics rather
than being conflated with cycle counts. `analyze_concurrent.py` calculates median
ratios and 90% independent bootstrap intervals with 20,000 deterministic resamples.
It resamples **repetitions**, not the correlated RTT observations inside each run.
A one-sided bound must fit inside the limit to mark that metric `within_limit`;
an interval wholly beyond the limit is `regression`, otherwise `inconclusive`.
Both independent sets must support a conclusion. Merely failing to detect a
difference is not evidence of parity.

The JSON analysis records its own script and raw-input hashes. Undefined or
unbounded diagnostic intervals (for example, CPU-time ratios with zero baseline
samples) use `null`, not invalid JSON numeric infinities.

These small-sample, per-case intervals are exploratory and do not provide a
simultaneous family-wise guarantee across the entire matrix. A clear improvement
is acceptable; this is a non-regression gate, not a demand for exact equality.
The [five-minute CI gate](gate.md) uses seven paired ABBA/BAAB blocks of 250 ms
windows per case, plus interleaved A/A controls and matched Asio diagnostics.
Both Task policies are warmed before the measured sequence; no extra
warmup exchanges are inserted between its windows. It bootstraps whole paired
blocks, not independent repetitions. It retains the workload matrix, thresholds,
optional CPU placement, and setup-through-analysis process-tree watchdog. Failed
same-code controls invalidate a run; unresolved candidate intervals cannot pass.
Do not conflate results from this paired protocol with the legacy research protocol
above. Raw results are ignored local or separately published artifacts, not bundled
with the source checkout.

## Correctness coverage

The added tests run under both scheduler modes and with successful-completion
skipping both enabled and disabled:

- 1024 simultaneously connected clients, nested Task chains, synthetic errors,
  explicit recovery, repeated subsequent I/O, verified payloads, and remote close.
- Concurrent join-all failures with pending reads into buffers borrowed from
  parent frames, followed by cancellation, error propagation, and destruction.
- Runtime shutdown with pending TCP reads and half the join handles dropped.
  Result observers verify destruction precedes completion publication; pinned
  inspection roots check submitted/completed counts before worker contexts die.

The echo fixture's abrupt close can produce either EOF or WSAECONNRESET on
Windows; both are checked rather than assuming every close is graceful. Existing
clean half-close, partial I/O, AcceptEx shutdown, and 20,000-deep failure tests
remain in the suite. Debug, Release, and ASan testing are complementary evidence,
not proof of race freedom. This section's historical measurements are Windows-only.
The [Linux backend](linux.md) now runs the same sharded-runtime correctness cases;
no cross-platform performance comparison is claimed.
