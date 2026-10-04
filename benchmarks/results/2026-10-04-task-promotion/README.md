# Native Task Promotion

The prototype is now the sole library, named `weave`. The former coroutine
implementation, socket adapters, derived Context, and runtime bridges are gone.
Socket operations, synchronous roots, and spawned runtime roots use native
`weave::Task<T>`. The IOCP transport and scheduler algorithms are retained.

## Result

Correctness passes; **the performance gate did not pass**.

- Debug, Release, and MSVC AddressSanitizer: all 11 CTest targets passed in each
  configuration, including the four echo-server integrations.
- Release core: 54 cases / 95,690 assertions. ASan core: 55 cases / 95,697
  assertions, including its allocator-poisoning case. Benchmark fixtures: 15
  cases / 4,219 assertions. All 156 main benchmark cases passed their smoke run.
- Gate checker: 21 synthetic tests passed, including historical regression,
  uncertainty, and metadata-mismatch checks. Supervisor and native positive
  control tests passed.
- Performance gate: **234.19 seconds**, below the hard 300-second deadline;
  756 windows and 24,710,646 verified roundtrips. No retries or dropped windows.
- Same-code controls: 30/36 passed. Six failures make the overall decision
  `measurement_unreliable` (exit 5), not parity or a performance pass.
- Current explicit-result versus automatic-propagation Tasks: 29/36 checks
  passed, zero regression flags, seven inconclusive. These are not old-versus-new
  implementation checks.
- Historical Task comparison: 13/36 passed, 21 inconclusive, two p99 regression
  flags. These flags remain visible despite the failed overall controls. They
  must not be silently converted to a pass or attributed conclusively to code.

The API migration was explicitly requested. Its completion does not establish
performance parity or production readiness.

## Throughput

Before is the saved automatic-Task **prototype**, not the former Async API.
After is the promoted native Task implementation. Both use seven blocks per
case, two 250 ms candidate windows per block; figures below are medians of the
seven within-block geometric means. Asio is the current run's median of seven
250 ms diagnostic windows. Units are **thousands of verified roundtrips/second**.
These descriptive figures are retained even though the controls rejected the run.

| Scheduler | Connections / workers | Payload / CPU steps | Before | After | Change | Matched Asio | After CV | Asio CV |
|---|---|---|---:|---:|---:|---:|---:|---:|
| Affine | 64 / 4 | 1 KiB / 0 | 197.44 | 192.80 | -2.35% | 192.34 | 2.3% | 2.1% |
| Affine | 1024 / 1 | 1 KiB / 0 | 59.29 | 56.05 | -5.46% | 56.82 | 3.3% | 4.9% |
| Affine | 1024 / 4 | 1 KiB / 0 | 180.73 | 170.72 | -5.54% | 175.80 | 12.8% | 4.9% |
| Affine | 1024 / 8 | 1 KiB / 0 | 172.95 | 172.04 | -0.53% | 176.62 | 2.6% | 0.9% |
| Affine | 256 / 4 | 64 KiB / 0 | 11.18 | 11.08 | -0.90% | 11.57 | 3.9% | 2.7% |
| Affine | 1024 / 4 | 1 KiB / 512 | 183.44 | 179.43 | -2.19% | 165.22 | 4.6% | 2.6% |
| Stealing | 64 / 4 | 1 KiB / 0 | 190.08 | 188.00 | -1.09% | 176.64 | 0.9% | 1.9% |
| Stealing | 1024 / 1 | 1 KiB / 0 | 59.54 | 56.38 | -5.31% | 58.71 | 2.9% | 3.0% |
| Stealing | 1024 / 4 | 1 KiB / 0 | 185.44 | 176.40 | -4.88% | 158.86 | 2.4% | 1.9% |
| Stealing | 1024 / 8 | 1 KiB / 0 | 177.52 | 169.93 | -4.28% | 158.99 | 1.3% | 3.9% |
| Stealing | 256 / 4 | 64 KiB / 0 | 11.17 | 11.18 | +0.04% | 10.51 | 1.5% | 1.2% |
| Stealing | 1024 / 4 | 1 KiB / 512 | 186.16 | 178.76 | -3.97% | 60.44 | 1.2% | 56.9% |

CV describes the individual windows, not the estimator's confidence interval.
The last Asio case is especially unstable; do not interpret it as a Weave win.
Asio uses sharded contexts for affine rows and a shared context for stealing rows.

## CPU And Tail Latency

The same historical block estimator is used here. Cycles/RTT is client process
CPU cycles per completed roundtrip, not CPU time. p99 is a per-window percentile,
not pooled p99 over the run. Units for latency are **milliseconds**.

| Scheduler | Connections / workers / payload / CPU steps | Cycles before | Cycles after | Change | p99 before | p99 after | Change |
|---|---|---:|---:|---:|---:|---:|---:|
| Affine | 64 / 4 / 1 KiB / 0 | 85,608 | 87,379 | +2.1% | 0.53 | 0.53 | +0.1% |
| Affine | 1024 / 1 / 1 KiB / 0 | 73,778 | 77,569 | +5.1% | 19.18 | 19.79 | +3.2% |
| Affine | 1024 / 4 / 1 KiB / 0 | 93,792 | 94,550 | +0.8% | 6.48 | 9.59 | +48.1% |
| Affine | 1024 / 8 / 1 KiB / 0 | 105,775 | 106,122 | +0.3% | 7.01 | 8.71 | +24.3% |
| Affine | 256 / 4 / 64 KiB / 0 | 449,222 | 487,686 | +8.6% | 24.47 | 28.29 | +15.6% |
| Affine | 1024 / 4 / 1 KiB / 512 | 92,423 | 93,755 | +1.4% | 6.51 | 6.83 | +4.8% |
| Stealing | 64 / 4 / 1 KiB / 0 | 92,055 | 92,974 | +1.0% | 0.64 | 0.64 | +0.4% |
| Stealing | 1024 / 1 / 1 KiB / 0 | 73,681 | 77,203 | +4.8% | 18.49 | 20.54 | +11.1% |
| Stealing | 1024 / 4 / 1 KiB / 0 | 93,842 | 97,986 | +4.4% | 7.10 | 8.19 | +15.3% |
| Stealing | 1024 / 8 / 1 KiB / 0 | 143,260 | 146,148 | +2.0% | 7.24 | 8.50 | +17.5% |
| Stealing | 256 / 4 / 64 KiB / 0 | 742,480 | 759,765 | +2.3% | 24.24 | 26.02 | +7.3% |
| Stealing | 1024 / 4 / 1 KiB / 512 | 93,530 | 97,016 | +3.7% | 7.07 | 8.05 | +13.8% |

The two historical p99 regression flags are affine 1024/4/1 KiB (+48.1%,
90% interval +35.8% to +74.9%) and stealing 1024/8/1 KiB (+17.5%, interval +12.0%
to +19.7%). Cross-run drift is not cancelled by this independent comparison.
Current p99 window CV reaches 156.4% in the former case; the latter has 4.9% CV.
Failed controls do not justify dismissing either flag, but prevent overall parity
certification. All confidence intervals and failed controls are in `gate.json`.

CPU seconds and core equivalents are retained separately in the raw data. Some
CPU-time windows report zero despite nonzero process cycles; these counters do
not certify CPU-time parity. No conversion from cycles to CPU seconds is made.

## Provenance And Reproduction

AMD Ryzen 9 9900X, Windows 11 build 22631, MSVC 19.44.35221.0, Release without
ASan or profiling. Eight client and four peer physical cores, one logical CPU
per core, with matching masks/hardware/OS/power scheme in the archived baseline.
The baseline is [the previous paired prototype run](../2026-10-04-paired-ci-gate/README.md).

```powershell
cmake --build --preset release --parallel
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-concurrent-gate.ps1 -IsolateCpus -BaselineDirectory benchmarks/results/2026-10-04-paired-ci-gate -OutputDirectory out/new-promotion-check
```

The watchdog covers metadata collection through historical analysis; builds and
correctness tests are separate. Source/executable hashes and gate provenance
were verified against disk after the run with zero mismatches. See
[run.json](run.json), [gate.json](gate.json), [paired.json](paired.json),
[environment.json](environment.json), and [current protocol](../../../docs/gate.md).
All evidence is preserved; no timing outcome was used to change thresholds,
select windows, or silently replace the baseline.
