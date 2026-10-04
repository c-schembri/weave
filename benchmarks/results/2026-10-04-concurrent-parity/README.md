# Concurrent Task parity: 2026-10-04

> Historical prototype evidence. Labels, commands, and raw hashes describe the
> original code. That source has since been replaced by native `weave::Task`;
> linked methodology documents now describe the current gate.

## Decision

The target is better ergonomics with networking parity, not a TCP speedup.
The concurrent correctness tests pass, and most throughput/CPU-cycle/p99 medians
are close. **The full non-regression gate is not cleared.** Weave 1 remains the
default; these results neither justify replacing it yet nor reject the Task model.

The clearest remaining performance question is 64 KiB traffic under work
stealing. The longer, unchanged-binary [followup](../2026-10-04-concurrent-parity-focused/README.md)
measured throughput changes of -8.2% and -4.7%, with wide intervals. The apparent
eight-worker p99 penalty did not persist in those longer medians. Neither finding
establishes a cause. CPU-time accounting was too variable to certify CPU-seconds
parity; independently collected cycle counts remain useful cost evidence.

There were 504 broad-matrix trials and 67,692,155 verified roundtrips, with no
reported errors and progress on every connection. Including the followup gives
672 trials and 113,971,884 verified roundtrips. This is substantial test evidence,
not proof of lifetime correctness, race freedom, or production latency guarantees.

## Configuration

- Ryzen 9 9900X, 12 physical / 24 logical processors; Windows 11 Pro 22631.
- MSVC 19.44.35221.0, x64 Release, exceptions disabled, frame recycling enabled.
- ASan and runtime profiling off for timing; original IOCP and allocator unchanged.
- Pinned Asio revision: `366dfc44640182cb21c1ebf7efb658a6bec13f5a`.
- Two independent, randomly interleaved sets; 36 cases x 7 repetitions per set.
- Each repetition has a one-second measurement window plus final exchange drain.
- Four-worker echo peer in a separate process; client and peer CPU counted separately.
- Frequency and affinity not fixed; shared development host. No builds or tests
  ran concurrently with the measurements.

See [methodology](../../../docs/concurrent.md) for measurement boundaries and limitations.
These are persistent-connection, closed-loop loopback workloads, not an open-loop
offered-load test. Latency includes client validation and any configured CPU work.
Asio's sharded contexts are matched to affine Weave and its shared context to
work-stealing Weave; the scheduling algorithms are not claimed to be equivalent.

## Primary Results

All values below are medians of seven repetitions from `comparison.json`.
Changes are Weave 2 / Weave 1 - 1. Higher throughput is better; lower CPU cost and
latency are better. A latency median is the median of per-trial percentiles, not a
pooled percentile. `Mixed` adds 512 integer-work steps to each 1 KiB response.

### Throughput

Units: thousands of validated roundtrips per second.

| Scheduler | Connections / workers / payload | Weave 1 | Weave 2 | Change | Asio |
|---|---|---:|---:|---:|---:|
| Affine | 64 / 4 / 1 KiB | 192.26 | 194.71 | +1.3% | 189.13 |
| Affine | 1024 / 1 / 1 KiB | 52.78 | 53.43 | +1.2% | 53.87 |
| Affine | 1024 / 4 / 1 KiB | 185.09 | 188.17 | +1.7% | 179.52 |
| Affine | 1024 / 8 / 1 KiB | 181.90 | 183.32 | +0.8% | 180.80 |
| Affine | 256 / 4 / 64 KiB | 14.25 | 14.83 | +4.1% | 14.28 |
| Affine | 1024 / 4 / Mixed | 187.06 | 178.76 | -4.4% | 175.46 |
| Stealing | 64 / 4 / 1 KiB | 187.64 | 186.53 | -0.6% | 170.89 |
| Stealing | 1024 / 1 / 1 KiB | 53.59 | 54.47 | +1.6% | 52.93 |
| Stealing | 1024 / 4 / 1 KiB | 182.59 | 181.59 | -0.5% | 167.98 |
| Stealing | 1024 / 8 / 1 KiB | 189.75 | 187.76 | -1.1% | 177.70 |
| Stealing | 256 / 4 / 64 KiB | 15.09 | 15.29 | +1.4% | 12.98 |
| Stealing | 1024 / 4 / Mixed | 183.75 | 178.70 | -2.7% | 159.20 |

### CPU Cost and Tail Latency

CPU cost is **thousands of client process cycles per roundtrip**, not CPU time.
p99 is in milliseconds. The peer's CPU is excluded from these client numbers.

| Scheduler | Connections / workers / payload | Cycles W1 | Cycles W2 | Cycles Asio | Change | p99 W1 | p99 W2 | p99 Asio | Change |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Affine | 64 / 4 / 1 KiB | 88.0 | 87.3 | 91.2 | -0.8% | 0.504 | 0.504 | 0.506 | +0.0% |
| Affine | 1024 / 1 / 1 KiB | 83.1 | 82.1 | 81.4 | -1.2% | 21.301 | 20.934 | 21.780 | -1.7% |
| Affine | 1024 / 4 / 1 KiB | 93.7 | 92.2 | 96.6 | -1.6% | 6.909 | 6.946 | 7.243 | +0.5% |
| Affine | 1024 / 8 / 1 KiB | 106.3 | 106.0 | 108.0 | -0.2% | 6.758 | 6.897 | 6.792 | +2.1% |
| Affine | 256 / 4 / 64 KiB | 470.7 | 451.4 | 524.9 | -4.1% | 26.694 | 26.732 | 27.073 | +0.1% |
| Affine | 1024 / 4 / Mixed | 93.0 | 97.0 | 99.6 | +4.2% | 6.802 | 7.242 | 7.230 | +6.5% |
| Stealing | 64 / 4 / 1 KiB | 93.0 | 93.9 | 102.5 | +0.9% | 0.696 | 0.697 | 0.474 | +0.2% |
| Stealing | 1024 / 1 / 1 KiB | 81.8 | 80.5 | 82.9 | -1.6% | 21.357 | 21.170 | 21.666 | -0.9% |
| Stealing | 1024 / 4 / 1 KiB | 95.8 | 96.5 | 103.8 | +0.7% | 7.451 | 7.574 | 6.954 | +1.7% |
| Stealing | 1024 / 8 / 1 KiB | 135.8 | 140.2 | 127.5 | +3.2% | 6.459 | 6.935 | 6.836 | +7.4% |
| Stealing | 256 / 4 / 64 KiB | 713.2 | 696.0 | 485.4 | -2.4% | 25.920 | 25.999 | 27.648 | +0.3% |
| Stealing | 1024 / 4 / Mixed | 95.4 | 98.1 | 110.2 | +2.9% | 7.332 | 7.461 | 7.407 | +1.8% |

### CPU-Time Diagnostics

The following are client CPU core equivalents from `GetProcessTimes`
(process CPU seconds / wall seconds). These observations are retained, **not
accepted as stable CPU-usage estimates**. Quantization, zero samples, and large
between-repetition variation make their ratios unreliable. Cycle counts are a
different measurement and must not be converted to seconds to fill this gap.
The raw data also contains CPU microseconds per operation and separate peer CPU.

| Scheduler | Connections / workers / payload | Weave 1 | Weave 2 | Asio |
|---|---|---:|---:|---:|
| Affine | 64 / 4 / 1 KiB | 0.781 | 0.594 | 1.156 |
| Affine | 1024 / 1 / 1 KiB | 0.795 | 0.795 | 0.748 |
| Affine | 1024 / 4 / 1 KiB | 2.211 | 2.727 | 2.898 |
| Affine | 1024 / 8 / 1 KiB | 0.436 | 0.342 | 0.202 |
| Affine | 256 / 4 / 64 KiB | 0.015 | 0.077 | 0.154 |
| Affine | 1024 / 4 / Mixed | 3.161 | 2.662 | 3.146 |
| Stealing | 64 / 4 / 1 KiB | 1.687 | 1.406 | 2.953 |
| Stealing | 1024 / 1 / 1 KiB | 0.859 | 0.827 | 0.842 |
| Stealing | 1024 / 4 / 1 KiB | 3.303 | 3.116 | 3.262 |
| Stealing | 1024 / 8 / 1 KiB | 0.358 | 0.419 | 0.357 |
| Stealing | 256 / 4 / 64 KiB | 0.170 | 0.249 | 0.154 |
| Stealing | 1024 / 4 / Mixed | 3.116 | 3.029 | 2.980 |

## Independent Confirmation

Changes in medians from the second seven-repeat set, `confirmation.json`:

| Scheduler | Connections / workers / payload | Throughput | Client cycles/op | p99 |
|---|---|---:|---:|---:|
| Affine | 64 / 4 / 1 KiB | +1.6% | -2.2% | -2.8% |
| Affine | 1024 / 1 / 1 KiB | +21.5% | -18.9% | +6.5% |
| Affine | 1024 / 4 / 1 KiB | +6.1% | -3.3% | -1.8% |
| Affine | 1024 / 8 / 1 KiB | -1.2% | -0.1% | +5.6% |
| Affine | 256 / 4 / 64 KiB | -0.3% | +0.9% | +3.2% |
| Affine | 1024 / 4 / Mixed | +0.0% | -0.4% | -2.3% |
| Stealing | 64 / 4 / 1 KiB | -0.8% | +0.5% | +0.8% |
| Stealing | 1024 / 1 / 1 KiB | +1.9% | -1.9% | +2.3% |
| Stealing | 1024 / 4 / 1 KiB | +4.0% | -3.3% | -0.8% |
| Stealing | 1024 / 8 / 1 KiB | -3.1% | +0.7% | +13.6% |
| Stealing | 256 / 4 / 64 KiB | -8.5% | -1.2% | +1.6% |
| Stealing | 1024 / 4 / Mixed | -0.2% | +0.3% | -3.5% |

## Uncertainty and Outliers

The provisional gate was selected before the repeated measurements: at most 5%
less throughput, 5% more client cycles/op, and 10% more p99 latency. The analyzer
uses 90% independent bootstrap intervals for median ratios, resampling seven
repetitions rather than millions of correlated RTTs. These are exploratory
per-case intervals, not a simultaneous guarantee across all cases.

Only the 64-connection/four-worker affine case clears all three interval bounds
in **both broad sets**. No interval establishes a definite regression, but many
are inconclusive. That distinction matters: no detected regression is not parity.
The apparent +21.5% single-worker gain in confirmation is not a supported win.

Maximum coefficients of variation across the Weave cases:

| Set | Throughput CV | Cycles/op CV | p99 CV |
|---|---:|---:|---:|
| Primary | 28.2% | 9.2% | 246.8% |
| Confirmation | 31.7% | 25.2% | 14.8% |

One primary Weave 2 affine 1024-connection/four-worker trial had p99 **683.458 ms**
and maximum latency 684.920 ms, versus a 6.946 ms median p99. It remains in the raw
data and analysis. Its cause is not established; it cannot honestly be discarded
or assigned to host noise. The corresponding spike did not repeat in confirmation,
whose highest p99 across all cases was 29.861 ms. The primary Asio shared 64 KiB
case also had 215.614 ms and 95.434 ms p99 outliers. Median tables alone are not
sufficient evidence about rare stalls.

The [longer followup](../2026-10-04-concurrent-parity-focused/README.md) preserves
both additional sets, including the less favorable one. It does not retroactively
replace these results. Do not keep rerunning until a favorable subset appears.

## Correctness Validation

The prototype suite now has **18 cases and 85,534 assertions**, all passing.
Three added cases exercise both schedulers and both IOCP successful-completion
skipping settings: 1024 concurrent connections with nested Task failures/recovery;
256 concurrent join-all/cancellation sessions borrowing parent buffers; and 256
pending reads during shutdown with half the join handles dropped. Destruction and
completion accounting are checked. Existing deep propagation and I/O tests remain.

| Validation | Result |
|---|---|
| Full Debug CTest | 6/6 passed |
| Full Release CTest | 6/6 passed |
| ASan Release, three prototype CTest entries, three consecutive runs each | 9/9 executions passed |
| Broad and focused measured trials | 672/672 without reported errors |

```powershell
cmake --build out/weave2 --config Debug --parallel
ctest --test-dir out/weave2 -C Debug --output-on-failure
cmake --build out/weave2 --config Release --parallel
ctest --test-dir out/weave2 -C Release --output-on-failure
cmake --build out/weave2-asan --config Release --target weave2_tests weave2_bench weave2_concurrent --parallel
ctest --test-dir out/weave2-asan -C Release -R '^weave2_' --repeat until-fail:3 --output-on-failure
```

The sanitizer build uses `WEAVE_ENABLE_ASAN=ON`. The repeated ASan run was limited
to the three prototype entries, not the entire original-library test suite.
No Linux, alternate-compiler, or thread-sanitizer result is claimed.

## Evidence and Reproduction

```powershell
./weave2/bench-concurrent.ps1 -OutputDirectory weave2/results/2026-10-04-concurrent-parity -DurationMilliseconds 1000 -Repetitions 7
```

Use a new output directory when reproducing; the script refuses to overwrite
existing results. Raw evidence: [primary](comparison.json),
[confirmation](confirmation.json), [analysis](analysis.json),
[environment and pre-run source/binary hashes](environment.json).
The executable is identical in the broad and focused measurements.

Analysis was regenerated afterward only to encode undefined/unbounded diagnostic
ratios as JSON `null` rather than nonstandard numeric infinities, and to record
postprocessing provenance. The analysis includes the current analyzer hash and
both raw-input hashes; the environment snapshot retains the original analyzer
hash. Measurements, bootstrap computation, limits, and gate classifications did
not change. All other recorded source and executable hashes still match.

Before default adoption, investigate the large-payload stealing difference and
unexplained stalls on a more controlled host, and obtain stable CPU-time evidence.
Do not optimize the unchanged TCP engine merely to manufacture a Task speedup.
