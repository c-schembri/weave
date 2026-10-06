# Four-worker runtime stress comparison

This is the heavier, manually invoked four-worker Windows TCP server comparison.
The same servers and load generator also support the separate
[local runtime scaling sweep](../../../../docs/runtime-scaling.md).
Weave, Tokio and the pinned Asio control have four I/O workers. An identical
separate Asio client process uses eight workers. Four server cores and eight
client cores are selected from different physical cores; no SMT core is shared.
The host needs twelve accessible physical cores in one Windows processor group.
CPU placement does not reserve those cores against other applications.

Weave uses work stealing with its default sharded IOCP layout. Tokio 1.53.2 uses
[`Builder::new_multi_thread().worker_threads(4).enable_io()`](https://docs.rs/tokio/1.53.2/tokio/runtime/struct.Builder.html#method.worker_threads); its accept loop also
runs on a worker, not the calling `block_on` thread. Asio uses one `io_context`
run by four threads. These are complete library/runtime comparisons, not isolated
scheduler comparisons: their Windows socket backends and allocation strategies
are different. No library implementation is modified or special-cased.

The protocol reads a complete fixed-size request, optionally performs integer
work, and writes the complete response. Every connection has one outstanding
request and a reusable buffer. TCP_NODELAY is enabled on both ends, the requested
backlog is 8192, and every returned byte is validated. On Windows all three servers
now encode that request as `SOMAXCONN_HINT(8192)`, rather than passing a plain
positive value that the OS can silently cap. Earlier archived measurements used
the plain value; their actual queue capacity was not 8192 on this host. See the
[backlog investigation](../../../../docs/tcp-backlog.md). CPU work uses identical wrapping
64-bit arithmetic and returns its result in the first eight bytes, so the work
cannot be optimized away. It is not an HTTP or raw chunked-echo benchmark.

| Connections | Frame | Server CPU work |
| ---: | ---: | --- |
| 1024 | 1 KiB | None |
| 4096 | 1 KiB | None |
| 256 | 64 KiB | None |
| 1024 | 1 KiB | 20,000 dependent integer steps on every eighth connection |

Each fresh-process repetition sets up all connections and performs eight verified
warmup RTTs per connection before the measurement barrier. The default records
two seconds of sustained traffic, then completes every in-flight request. Setup,
warmup, sorting and process cleanup are excluded. Seven blocks randomize both
workload order and backend order with a recorded seed; all repetitions are retained.
An outer Windows job owns the supervisor and descendants from suspended startup,
with a five-minute default deadline. Timeouts fail rather than report partial
evidence as a complete result. The supervisor also has per-process phase deadlines.

Failures produce a per-window `.failure.json` with the phase, process IDs, exit
codes observed before cleanup, and stderr tails. Failure phases distinguish
socket cleanup and process exit. `STOP` is followed by a `CLOSED` acknowledgment
after all sockets close (25-second bound), then a
five-second process-exit wait. The outer job still has a five-minute default
deadline; cleanup is outside the measurement window.

The client reports the first failing connection, operation, error code/category,
and transferred/expected byte
counts. Setup timeouts include atomic stage counts and bounded per-connection
warmup snapshots; these are diagnostic snapshots, not a synchronized view of
all connections. Progress instrumentation runs during setup, not each measured
exchange. Server-side client failures are also logged for each backend. Failure
diagnostics do not retry windows, increase deadlines, or turn partial runs into
successful evidence.

Throughput is validated completions divided by measured wall time, including final
draining. Every completed RTT is recorded; percentiles use nearest-rank exact
quantiles. The report takes medians of per-repetition percentiles, not pooled tails.
Connection-local sample storage is reserved before measurement and can grow if
necessary. This instrumentation is identical across all servers.

This is closed-loop saturation, not an open-loop SLO/overload test. There is no
coordinated-omission correction. Loopback and the load generator can limit throughput;
client counters are retained, and changing client worker count provides a diagnostic.
Server cycles per completed request are reported independently of CPU-time cores.
`GetProcessTimes` has exhibited severe quantization/zero CPU-time deltas in these
network workloads on this host; those values are diagnostics, not proof of low
CPU cost. Cycles are never converted to CPU seconds. Process-private memory is
also recorded. Exploratory paired bootstrap intervals resample repetitions, not
individual correlated RTTs; they are not simultaneous guarantees over the matrix.

## Run

Rust/Cargo are only needed for this optional comparison. CMake does not require
or download a Rust toolchain, and neither Weave nor automatic correctness CI
depends on Tokio. Cargo.lock pins the Rust dependency graph; Cargo output goes
under the ignored root build directory:

```sh
cmake --preset windows
cmake --build --preset release --target weave_runtime_server weave_runtime_load --parallel 4
cargo build --release --locked --manifest-path modules/tcp/benchmarks/tokio/Cargo.toml --target-dir build/tokio
python scripts/bench_tokio.py
```

Do not run builds or other tests during measurement. Raw samples, logs, environment,
source/binary hashes and analysis live in ignored `benchmarks/results/tokio-*`.
Existing evidence is never overwritten. No production Weave changes are made by
this comparison, so a before/after performance delta is not applicable.

```sh
# Fast protocol/cleanup checks; not performance evidence (32 connections).
python scripts/bench_tokio.py --smoke --duration-ms 100 --repetitions 1 --timeout-seconds 60

# Same matched workload with shared IOCP as a separate configuration.
python scripts/bench_tokio.py --backends weave-shared tokio asio

# Load-generator capacity diagnostic (same eight physical client cores).
python scripts/bench_tokio.py --client-workers 16 --workloads 4096-small

python tests/tokio_benchmark.py
cargo test --locked --manifest-path modules/tcp/benchmarks/tokio/Cargo.toml --target-dir build/tokio
```

Synthetic Python evidence checks are registered in correctness CI; they neither
build Tokio nor execute networking measurements. Native stress/smoke runs are
manual only.

## Observed results: 2026-10-06

Windows 11, Ryzen 9 9900X (12 physical cores), MSVC 19.44 Release and Rust 1.94.0
Release; Tokio 1.53.2 and Asio 1.36.0. No production Weave source was changed.
The main matrix used seven randomized repetitions with two-second windows.
Throughput and p99 below are medians across repetitions, not pooled results.

| Connections / workload | Weave RTT/s | Tokio RTT/s | Asio RTT/s | Weave p99 ms | Tokio p99 ms | Asio p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1024, 1 KiB | 163,487 | 100,304 | 163,149 | 7.14 | 22.43 | 7.30 |
| 4096, 1 KiB | 148,469 | 89,473 | 150,114 | 30.19 | 150.59 | 28.13 |
| 256, 64 KiB | 73,360 | 54,925 | 60,164 | 4.41 | 8.68 | 5.42 |
| 1024, uneven CPU | 150,760 | 94,597 | 153,808 | 8.25 | 23.66 | 7.39 |

Median server CPU costs were 98.02 / 119.97 / 104.27 kcycles per RTT
(Weave / Tokio / Asio) for 1024 small connections; 107.51 / 128.74 / 115.34
for 4096; 223.27 / 264.28 / 275.26 for large frames; and
107.42 / 129.59 / 112.75 for uneven CPU work. CPU-time counters frequently
reported zero despite substantial cycle counts, so no CPU-seconds savings claim
is made. Cycle costs are process costs, not total system/kernel energy costs.

The medians hide significant cliffs: sharded Weave's throughput CV was 21.8%
at 4096 connections and 32.9% with uneven CPU work. Those runs included 162.63 ms
and 88.02 ms p99 respectively. Tokio also had a 404.48 ms p99 outlier at 4096.
All outliers remain in the analysis; no run was discarded for being slow.

A separate seven-repetition, one-second diagnostic compared both Weave I/O
layouts in the same randomized blocks, using the original eight client workers:

| Workload | Backend | Median RTT/s | Median p99 ms | Worst repetition p99 ms | Throughput CV |
| --- | --- | ---: | ---: | ---: | ---: |
| 4096 small | Weave sharded | 139,460 | 31.46 | 295.25 | 40.5% |
| 4096 small | Weave shared | 152,717 | 28.65 | 57.65 | 6.8% |
| 4096 small | Tokio | 88,503 | 146.46 | 152.10 | 4.0% |
| 4096 small | Asio | 146,192 | 29.56 | 164.13 | 12.0% |
| Uneven CPU | Weave sharded | 145,922 | 8.30 | 146.87 | 42.5% |
| Uneven CPU | Weave shared | 164,889 | 7.38 | 63.14 | 19.3% |
| Uneven CPU | Tokio | 95,639 | 23.39 | 24.99 | 2.6% |
| Uneven CPU | Asio | 147,618 | 7.63 | 55.68 | 14.2% |

Sharded Weave's exploratory paired throughput-ratio intervals against Tokio in
this diagnostic were [0.801, 1.607] and [0.547, 1.613], crossing parity. Shared
Weave's were [1.604, 1.753] and [1.699, 1.742]. Shared performed better here,
but also had outliers; this does not isolate their cause or justify changing the
default from a single host's diagnostic. Occasional Asio cliffs also caution
against attributing every stall exclusively to Weave.

Doubling the load generator to sixteen threads on the same eight physical cores
did not increase throughput in another one-second, seven-repetition 4096-client
diagnostic: Weave 139,445 RTT/s, Tokio 87,539, Asio 139,429. This is a capacity
check, not a claim that loopback or the client cannot limit other workloads.

Across the main matrix and both diagnostics: 161 measured windows, 28,780,920
validated RTTs and approximately 366.9 GiB of application request/response data.
Every connection completed measured requests; the overall minimum was six.
These are Windows loopback closed-loop results, not universal runtime rankings
or production latency guarantees. A consistent default-sharded win is not established.

Local raw evidence is ignored by Git:

- `benchmarks/results/tokio-20261006-122829/`: main matrix.
- `benchmarks/results/tokio-20261006-123312/`: sixteen-client-worker diagnostic.
- `benchmarks/results/tokio-20261006-123437/`: sharded/shared diagnostic.

Each contains environment/provenance, per-repetition samples, process logs and
analysis. Share those artifacts separately when publishing these numbers.

## Rerun: 2026-10-06, 13:37-13:47 local

The same main matrix and both diagnostics were rerun, with seven repetitions
per case. The native server and client executable hashes match the original
run exactly. Tokio was rebuilt with the same locked dependencies and toolchain
from the current formatted source; its binary hash changed. No production code
was changed. Previous/current differences are rerun variation, not optimization
gains. The main matrix completed within its five-minute deadline.

Main throughput medians, in validated RTT/s:

| Workload | Previous Weave | Rerun Weave | Change | Rerun Tokio | Rerun Asio |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1024, 1 KiB | 163,487 | 165,339 | +1.1% | 101,074 | 168,687 |
| 4096, 1 KiB | 148,469 | 149,027 | +0.4% | 92,508 | 151,464 |
| 256, 64 KiB | 73,360 | 75,524 | +2.9% | 56,905 | 64,325 |
| 1024, uneven CPU | 150,760 | 153,914 | +2.1% | 95,291 | 155,331 |

| Workload | Weave p99 ms | Tokio p99 ms | Asio p99 ms | Weave throughput CV |
| --- | ---: | ---: | ---: | ---: |
| 1024, 1 KiB | 6.71 | 22.59 | 6.44 | 0.6% |
| 4096, 1 KiB | 29.21 | 139.61 | 28.11 | 0.4% |
| 256, 64 KiB | 3.98 | 8.43 | 4.59 | 1.6% |
| 1024, uneven CPU | 7.26 | 23.21 | 6.98 | 1.0% |

Main median server costs were 97.63 / 119.32 / 103.13 kcycles per RTT
(Weave / Tokio / Asio), 107.32 / 126.66 / 114.47, 219.37 / 259.12 / 268.67,
and 106.00 / 128.89 / 112.01, respectively. CPU-time counters remain unreliable;
these cycle measurements do not establish CPU-seconds savings.

The sixteen-client-thread diagnostic returned 147,707 / 92,355 / 149,688 RTT/s
(Weave / Tokio / Asio), again not increasing throughput over the main matrix.
The completed sharded/shared diagnostic returned:

| Workload | Weave sharded RTT/s | Weave shared RTT/s | Tokio RTT/s | Asio RTT/s |
| --- | ---: | ---: | ---: | ---: |
| 4096, 1 KiB | 149,454 | 164,771 | 95,070 | 150,569 |
| 1024, uneven CPU | 153,974 | 167,299 | 97,611 | 155,051 |

The earlier large measured latency cliffs did not recur in the completed runs.
Worst sharded-Weave repetition p99 was 34.39 ms at 4096 connections and 7.87 ms
with uneven CPU work across these runs. This does not establish that the earlier
stalls are fixed: one sharded/shared attempt failed during setup or warmup for
the final 4096-client Weave repetition, after 53 completed windows. The client
reported only "Load generator setup or warmup failed"; the server log was empty.
Its cause remains unresolved. That attempt is retained as failed evidence and
excluded from the complete-run summaries, not silently treated as a pass.
The entire diagnostic was retried and completed successfully.

The three complete runs total 161 windows, 30,999,707 validated RTTs and
approximately 390.3 GiB of application request/response payload. Every connection
made measured progress; the minimum was nine RTTs. Raw evidence remains ignored:

- `benchmarks/results/tokio-20261006-133707/`: complete main matrix.
- `benchmarks/results/tokio-20261006-134137/`: complete sixteen-thread diagnostic.
- `benchmarks/results/tokio-20261006-134236/`: failed sharded/shared attempt.
- `benchmarks/results/tokio-20261006-134502/`: complete sharded/shared retry.
