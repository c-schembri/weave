# Paired CI gate, 2026-10-04

> Historical prototype evidence. Labels, commands, and raw hashes describe the
> original code. That source has since been replaced by native `weave::Task`;
> linked methodology documents now describe the current gate.

**Inconclusive, exit 2. Elapsed 230.477 seconds against a 300-second cap.**
One planned run, no retries, no discarded windows, unchanged regression limits.
756 measured windows, 25,639,907 verified roundtrips. Every connection progressed.

- Same-code A/A controls: **36/36 pass** (two-sided equivalence).
- Throughput: **12/12 pass** (no more than 5% loss).
- Client cycles/roundtrip: **12/12 pass** (no more than 5% increase).
- Per-window p99: **10/12 pass**, two inconclusive (10% limit).
- Established regressions: zero. This is **not** an overall parity pass.

Only the measurement harness/protocol changed. No Task, TCP, IOCP, allocator, or
runtime implementation was optimized to obtain these results. Weave 1 remains
the public default; `examples/echo.cpp` was not touched.

## Method

[Protocol](../../../docs/gate.md): six workloads, both scheduler modes. Weave 1 and Weave
2 reuse the exact same warmed runtime, sockets, peer, and buffers. Seven A/B
blocks and seven interleaved A/A blocks per case; each block uses four 250 ms
windows in balanced ABBA/BAAB order. Both labels in A/A call Weave 1. Asio runs
seven diagnostic windows on its own warmed/reused fixture, sharded for affine
and shared for stealing. Placement: eight client/four peer physical cores, one
logical processor per core. No build or test workload ran alongside this gate.

Values below are median window measurements (14 windows per Weave variant,
seven for Asio). **Paired change is a different estimator**: the median of seven
ratios of within-block geometric means, not the ratio of the displayed medians.
Intervals are 90% percentile bootstrap intervals, 20,000 whole-block resamples.
This is typical closed-loop window behavior, not pooled run-wide latency or an
open-loop production SLO. Small-sample intervals have no family-wise guarantee.

Workload notation: connections / client workers / payload bytes / CPU steps.

## Throughput

Thousands of validated roundtrips/second. Higher is better.

| Scheduler | Workload | Weave 1 | Weave 2 | Asio | Paired change | 90% interval | Block ratio CV |
|---|---|---:|---:|---:|---:|---:|---:|
| affine | 64/4/1024/0 | 196.92 | 197.17 | 194.29 | +0.60% | [+0.36, +1.46]% | 0.75% |
| affine | 1024/1/1024/0 | 59.85 | 59.29 | 60.95 | -0.68% | [-3.09, +2.93]% | 15.17% |
| affine | 1024/4/1024/0 | 180.38 | 180.42 | 179.41 | +0.08% | [-0.80, +1.32]% | 8.00% |
| affine | 1024/8/1024/0 | 172.17 | 173.07 | 172.74 | +0.44% | [+0.21, +1.11]% | 6.06% |
| affine | 256/4/65536/0 | 11.17 | 11.17 | 11.23 | -0.37% | [-0.98, +0.42]% | 16.28% |
| affine | 1024/4/1024/512 | 184.84 | 183.27 | 181.44 | -0.52% | [-0.98, +0.56]% | 1.05% |
| stealing | 64/4/1024/0 | 189.75 | 189.83 | 175.49 | -0.18% | [-0.31, +0.16]% | 0.26% |
| stealing | 1024/1/1024/0 | 59.61 | 59.61 | 59.97 | +0.66% | [-1.47, +1.06]% | 1.26% |
| stealing | 1024/4/1024/0 | 185.02 | 185.09 | 173.42 | +0.44% | [+0.15, +1.31]% | 0.79% |
| stealing | 1024/8/1024/0 | 177.75 | 177.35 | 167.82 | +0.03% | [-0.98, +0.69]% | 0.80% |
| stealing | 256/4/65536/0 | 11.19 | 11.22 | 10.78 | -0.25% | [-0.46, +0.36]% | 0.73% |
| stealing | 1024/4/1024/512 | 187.66 | 187.56 | 164.10 | -0.51% | [-1.25, +1.15]% | 1.28% |

## CPU Cost

Thousands of client cycles per validated roundtrip. Lower is better. These are
`QueryProcessCycleTime` counts, not seconds or a CPU-frequency conversion.

| Scheduler | Workload | Weave 1 | Weave 2 | Asio | Paired change | 90% interval | Block ratio CV |
|---|---|---:|---:|---:|---:|---:|---:|
| affine | 64/4/1024/0 | 85.94 | 85.95 | 88.57 | -0.24% | [-0.73, +0.42]% | 0.65% |
| affine | 1024/1/1024/0 | 73.27 | 73.96 | 71.97 | +0.75% | [-0.77, +3.15]% | 3.29% |
| affine | 1024/4/1024/0 | 93.65 | 93.68 | 95.38 | +0.32% | [-0.78, +0.77]% | 1.80% |
| affine | 1024/8/1024/0 | 105.43 | 105.46 | 108.60 | -0.21% | [-1.01, +0.19]% | 3.42% |
| affine | 256/4/65536/0 | 446.58 | 448.71 | 471.91 | +0.93% | [+0.10, +2.03]% | 1.71% |
| affine | 1024/4/1024/512 | 91.84 | 92.98 | 94.84 | -0.01% | [-0.28, +1.39]% | 1.38% |
| stealing | 64/4/1024/0 | 92.30 | 92.21 | 99.72 | +0.09% | [-0.34, +0.44]% | 0.42% |
| stealing | 1024/1/1024/0 | 73.62 | 73.62 | 73.14 | -0.70% | [-1.06, +1.49]% | 1.28% |
| stealing | 1024/4/1024/0 | 94.05 | 94.02 | 100.70 | -0.23% | [-1.27, -0.16]% | 0.79% |
| stealing | 1024/8/1024/0 | 142.60 | 143.38 | 128.97 | +0.38% | [+0.10, +0.77]% | 0.32% |
| stealing | 256/4/65536/0 | 726.40 | 738.94 | 497.54 | +0.68% | [-1.93, +3.48]% | 2.87% |
| stealing | 1024/4/1024/512 | 92.87 | 92.93 | 106.74 | +0.58% | [-0.92, +1.12]% | 1.27% |

`GetProcessTimes` CPU time remains a diagnostic, not a passed gate. Its median
reported client time was zero in both large-payload cases for all three
implementations despite nonzero cycle counts. Do not infer zero CPU work or
CPU-time parity. Raw client/peer time and cycle counters remain in `paired.json`.

## Tail Latency

Median per-window p99, milliseconds. Lower is better; not a pooled percentile.

| Scheduler | Workload | Weave 1 | Weave 2 | Asio | Paired change | 90% interval | Block ratio CV |
|---|---|---:|---:|---:|---:|---:|---:|
| affine | 64/4/1024/0 | 0.53 | 0.53 | 0.54 | -0.51% | [-2.22, +1.86]% | 3.37% |
| affine | 1024/1/1024/0 | 19.10 | 19.35 | 19.31 | +4.59% | [-1.23, +11.16]% | 27.72% |
| affine | 1024/4/1024/0 | 6.50 | 6.54 | 6.59 | +0.75% | [-5.66, +3.01]% | 28.90% |
| affine | 1024/8/1024/0 | 7.43 | 6.94 | 6.91 | -6.25% | [-13.51, -2.53]% | 15.59% |
| affine | 256/4/65536/0 | 24.38 | 24.47 | 24.38 | +0.52% | [-4.81, +15.78]% | 62.87% |
| affine | 1024/4/1024/512 | 6.51 | 6.42 | 6.67 | +1.32% | [-1.67, +8.14]% | 12.50% |
| stealing | 64/4/1024/0 | 0.64 | 0.64 | 0.48 | +0.79% | [-0.16, +1.09]% | 1.35% |
| stealing | 1024/1/1024/0 | 18.24 | 18.73 | 18.81 | +0.01% | [-3.96, +2.28]% | 3.73% |
| stealing | 1024/4/1024/0 | 7.06 | 7.12 | 7.34 | +0.46% | [-1.04, +1.56]% | 1.74% |
| stealing | 1024/8/1024/0 | 7.14 | 7.23 | 7.40 | +3.15% | [-1.35, +4.41]% | 3.95% |
| stealing | 256/4/65536/0 | 24.18 | 24.29 | 25.28 | -0.16% | [-5.19, +0.75]% | 4.01% |
| stealing | 1024/4/1024/512 | 7.02 | 7.08 | 6.78 | +2.15% | [-0.87, +7.86]% | 11.57% |

The two inconclusive rows are affine 1024/1/1 KiB and affine 256/4/64 KiB. Their
upper endpoints exceed the 10% limit. No threshold was widened to accept them.
Outlier blocks still matter: across cases, median A/B ratio CV is 1.16% for
throughput, 1.33% for cycles, and 7.79% for p99, but the maxima are 16.28%, 3.42%,
and 62.87%. Passing a median-based control is not proof of uniformly low jitter.

## Verification And Evidence

- Debug CTest: 9/9 passed, 49.48 seconds.
- Release CTest: 9/9 passed, 40.74 seconds.
- ASan RelWithDebInfo prototype CTest: 6/6 passed, 25.63 seconds.
- 17 synthetic gate cases include metric regressions, drift, A/A bias, incomplete
  matrices, bad counters, and CPU placement errors.
- Native positive-control smoke detects injected extra computation in both
  schedulers (Release check: 11.81x/11.85x client cycles/op). This is gross
  sensitivity verification, not a claim of 5% detection power in every workload.
- Supervisor tests cover exit propagation, log output, and killing a hung
  worker plus its child. No test workload overlapped the performance run.

`environment.json` preserves machine/build metadata and source/executable hashes;
`paired.json` has every window in execution order; `gate.json` has paired ratios,
intervals, all checks, diagnostics, and provenance; `run.json` records elapsed
time and exit status; `run.log` contains worker progress. Earlier independent
CI/research evidence is retained in its original directories and protocol.
