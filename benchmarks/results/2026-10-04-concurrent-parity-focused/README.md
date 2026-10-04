# Longer Concurrent Parity Followup: 2026-10-04

> Historical prototype evidence. Labels, commands, and raw hashes describe the
> original code. That source has since been replaced by native `weave::Task`;
> linked methodology documents now describe the current gate.

The [broad comparison](../2026-10-04-concurrent-parity/README.md) left two notable
questions: p99 at eight work-stealing workers, and 64 KiB work-stealing throughput.
This followup uses the **same executable and benchmark sources**, longer windows,
both schedulers, and both Asio baselines. It does not replace the broad evidence.

Two independent, randomly interleaved sets ran 12 cases x 7 repetitions, with a
three-second measurement window per trial plus final exchange drain. There were
168 trials, 46,279,729 verified roundtrips, zero reported errors, and progress on
every connection. Minimum connection progress was 120 exchanges in primary and
138 in confirmation. Configuration and measurement limitations are unchanged;
see [methodology](../../../docs/concurrent.md).

## Throughput

Medians of seven repetitions; thousands of roundtrips/second, higher is better.
Change is Weave 2 / Weave 1 - 1. Asio is sharded for affine and shared for stealing.

| Set | Scheduler | Connections / workers / payload | Weave 1 | Weave 2 | Change | Asio |
|---|---|---|---:|---:|---:|---:|
| Primary | Affine | 1024 / 8 / 1 KiB | 168.97 | 174.82 | +3.5% | 173.87 |
| Primary | Affine | 256 / 4 / 64 KiB | 13.37 | 13.32 | -0.4% | 14.32 |
| Primary | Stealing | 1024 / 8 / 1 KiB | 177.64 | 175.24 | -1.4% | 159.79 |
| Primary | Stealing | 256 / 4 / 64 KiB | 13.33 | 12.23 | -8.2% | 12.44 |
| Confirmation | Affine | 1024 / 8 / 1 KiB | 174.09 | 173.94 | -0.1% | 176.25 |
| Confirmation | Affine | 256 / 4 / 64 KiB | 14.71 | 15.91 | +8.2% | 15.99 |
| Confirmation | Stealing | 1024 / 8 / 1 KiB | 177.81 | 181.25 | +1.9% | 162.90 |
| Confirmation | Stealing | 256 / 4 / 64 KiB | 15.56 | 14.83 | -4.7% | 14.48 |

## CPU Cost and Tail Latency

CPU: thousands of client process cycles per roundtrip. p99: milliseconds.
Lower is better for both. CPU-time/core-equivalent diagnostics remain in the raw
data and analysis; their variability still precludes a CPU-seconds parity claim.

| Set | Scheduler / workload | Cycles W1 | Cycles W2 | Cycles Asio | Change | p99 W1 | p99 W2 | p99 Asio | Change |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Primary | Affine / 1024-8-1KiB | 112.8 | 111.2 | 112.8 | -1.4% | 8.009 | 7.220 | 7.338 | -9.8% |
| Primary | Affine / 256-4-64KiB | 505.4 | 502.9 | 497.5 | -0.5% | 27.712 | 27.590 | 28.087 | -0.4% |
| Primary | Stealing / 1024-8-1KiB | 144.4 | 145.8 | 135.5 | +1.0% | 7.306 | 7.395 | 8.235 | +1.2% |
| Primary | Stealing / 256-4-64KiB | 805.8 | 865.4 | 556.0 | +7.4% | 27.167 | 28.620 | 27.738 | +5.4% |
| Confirmation | Affine / 1024-8-1KiB | 109.6 | 109.6 | 112.6 | +0.0% | 7.267 | 7.216 | 7.124 | -0.7% |
| Confirmation | Affine / 256-4-64KiB | 484.6 | 486.9 | 510.0 | +0.5% | 27.505 | 27.066 | 27.138 | -1.6% |
| Confirmation | Stealing / 1024-8-1KiB | 143.8 | 145.5 | 134.0 | +1.1% | 7.263 | 7.270 | 7.705 | +0.1% |
| Confirmation | Stealing / 256-4-64KiB | 712.3 | 723.1 | 533.8 | +1.5% | 26.706 | 26.546 | 27.473 | -0.6% |

## Uncertainty

90% bootstrap intervals for percentage changes in median ratios are below.
The repetition is the resampling unit. The provisional non-regression bounds
remain -5% throughput, +5% cycles/op, +10% p99; both independent sets must support
a conclusion. These small-sample intervals are exploratory, not simultaneous
family-wise guarantees. See [analysis.json](analysis.json) for individual statuses
and coefficients of variation for Weave 1, Weave 2, and Asio.

| Set | Scheduler / workload | Throughput interval | Cycles/op interval | p99 interval | All three within limits? |
|---|---|---:|---:|---:|---|
| Primary | Affine / 1024-8-1KiB | [-11.1%, +17.8%] | [-7.1%, +6.5%] | [-23.4%, +10.7%] | No, inconclusive |
| Primary | Affine / 256-4-64KiB | [-13.5%, +33.5%] | [-14.6%, +15.6%] | [-4.1%, +8.5%] | No, inconclusive |
| Primary | Stealing / 1024-8-1KiB | [-20.8%, +14.7%] | [-4.6%, +11.9%] | [-16.8%, +48.6%] | No, inconclusive |
| Primary | Stealing / 256-4-64KiB | [-19.1%, +20.3%] | [-12.8%, +18.1%] | [-6.2%, +25.5%] | No, inconclusive |
| Confirmation | Affine / 1024-8-1KiB | [-3.6%, +4.3%] | [-1.3%, +3.8%] | [-13.2%, +4.3%] | Yes |
| Confirmation | Affine / 256-4-64KiB | [+2.7%, +17.0%] | [-3.9%, +2.3%] | [-2.0%, +1.1%] | Yes |
| Confirmation | Stealing / 1024-8-1KiB | [-3.6%, +4.2%] | [-1.8%, +3.3%] | [-9.3%, +7.3%] | Yes |
| Confirmation | Stealing / 256-4-64KiB | [-15.0%, +8.0%] | [-2.1%, +6.3%] | [-2.1%, +3.5%] | No, inconclusive |

Maximum CV across the Weave cases was 20.4% throughput, 19.3% cycles/op, and 22.4%
p99 in primary; 12.5%, 16.4%, and 7.8% in confirmation. Highest per-trial p99 across
all implementations was 47.174 ms and 30.712 ms respectively. No trials were
discarded. The more favorable confirmation does not erase primary uncertainty.

## Interpretation

- The eight-worker stealing p99 median changes were +1.2% and +0.1%, rather than
  the broad runs' +7.4% and +13.6%. The large apparent penalty did not persist.
  Primary intervals remain too wide to claim both-set non-regression.
- Large-payload stealing throughput was -8.2% and -4.7%; client cycles/op were
  +7.4% and +1.5%. Broad confirmation also had -8.5% throughput. This warrants
  investigation, but the intervals do not establish either a definite regression
  or parity. Task frame size/placement, scheduling, and peer/host effects are
  possible hypotheses, not measured causes.
- Three cases clear all three limits in confirmation alone; none clears all
  three in both focused sets. Keep the original API unchanged while narrowing
  the remaining questions. A coroutine microbenchmark win is not the criterion.

## Reproduction and Evidence

```powershell
./weave2/bench-concurrent.ps1 -OutputDirectory weave2/results/2026-10-04-concurrent-parity-focused -DurationMilliseconds 3000 -Repetitions 7 -Filter 'connections:1024/workers:8/bytes:1024/cpu:0/|connections:256/workers:4/bytes:65536/cpu:0/'
```

Choose a new output directory to reproduce without overwriting evidence.
[Primary](comparison.json), [confirmation](confirmation.json),
[analysis](analysis.json), [environment and pre-run source/binary hashes](environment.json).
As described in the broad report, analysis alone was subsequently reserialized
to represent non-finite diagnostic numbers as JSON `null` and add provenance.
No benchmark binary, raw sample, statistical computation, or classification changed.
