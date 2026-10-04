# Five-Minute CI Gate: 2026-10-04

> Historical prototype evidence. Labels, commands, and raw hashes describe the
> original code. That source has since been replaced by native `weave::Task`;
> linked methodology documents now describe the current gate.

**Completed in 252.8 seconds (4 min 13 sec), including setup and analysis.** An
external stopwatch measured 252.99 seconds for the complete PowerShell invocation.
The deadline was 300 seconds. No automatic retries or time extensions occurred.

The performance decision is **inconclusive**, exit code **2**: 45 of 72 checks
passed, zero established regressions, 27 inconclusive. This is not a parity pass.
All 504 planned trials completed without benchmark errors, with 21,369,660 verified
roundtrips and progress on every connection. The time-budget requirement is met;
the performance acceptance requirement is not yet met.

## Plan and Configuration

- [CI protocol](../../../docs/gate.md): all 36 cases, seven 300 ms repetitions in each of
  two independently randomized sets. The original workload matrix is retained,
  including both schedulers, 1/4/8 workers, 1024 connections, 64 KiB, mixed CPU/I/O,
  and both matched Asio baselines.
- Existing limits unchanged: at most 5% throughput loss, 5% more client cycles
  per roundtrip, and 10% more per-trial p99 latency. Every 90% bootstrap bound must
  satisfy its limit in both sets. Inconclusive evidence exits nonzero.
- Ryzen 9 9900X, Windows 11 Pro 22631, MSVC 19.44.35221.0, x64 Release. Exceptions,
  profiling, and ASan off; original IOCP/allocator/Task implementations unchanged.
- Process masks 5326100 (eight client physical cores) and 266305 (four peer
  physical cores), one logical processor per core. No client/peer SMT sharing.
- Shared development host, dynamic frequency, existing Ultimate Performance
  power plan unchanged. Other applications were not closed or reconfigured.
  Process affinity is not exclusive reservation of CPU resources.
- Pinned Asio: `366dfc44640182cb21c1ebf7efb658a6bec13f5a`. Affine Weave uses the
  sharded Asio baseline; work-stealing Weave uses shared Asio. Scheduler algorithms
  are not claimed identical.

These controlled-placement rates should be compared within this run, not to the
absolute rates from older unrestricted runs. Shorter trials buy bounded CI cost
at the expense of statistical power. CPU time remains diagnostic and is not
replaced by converting cycles to seconds. This closed-loop median-of-per-trial-p99
gate is not an open-loop SLO or maximum-stall guarantee.

## Primary Set

Medians of seven repetitions. Throughput is thousands of verified roundtrips/sec.
Changes are Weave 2 / Weave 1 - 1. Higher throughput is better; lower cycles and
p99 are better. Payload is 1 KiB unless stated; mixed adds 512 integer-work steps.

| Scheduler / connections / workers | Weave 1 | Weave 2 | Change | Asio | Cycles/op change | p99 change |
|---|---:|---:|---:|---:|---:|---:|
| Affine / 64 / 4 | 198.00 | 195.35 | -1.3% | 198.79 | -0.1% | +0.7% |
| Affine / 1024 / 1 | 91.41 | 86.14 | -5.8% | 79.91 | +6.1% | -3.3% |
| Affine / 1024 / 4 | 188.66 | 186.95 | -0.9% | 189.34 | +1.4% | +0.3% |
| Affine / 1024 / 8 | 179.04 | 179.34 | +0.2% | 177.69 | +0.1% | -1.9% |
| Affine / 256 / 4, 64 KiB | 11.45 | 11.45 | 0.0% | 11.44 | +0.6% | -1.1% |
| Affine / 1024 / 4, mixed | 189.05 | 192.66 | +1.9% | 192.03 | -1.5% | -0.8% |
| Stealing / 64 / 4 | 198.40 | 197.16 | -0.6% | 187.63 | -0.3% | +0.1% |
| Stealing / 1024 / 1 | 91.11 | 83.39 | -8.5% | 80.35 | +9.2% | +0.8% |
| Stealing / 1024 / 4 | 193.96 | 194.63 | +0.3% | 191.45 | -0.3% | -1.5% |
| Stealing / 1024 / 8 | 183.92 | 180.12 | -2.1% | 172.78 | +2.6% | +8.5% |
| Stealing / 256 / 4, 64 KiB | 11.67 | 11.75 | +0.7% | 11.06 | +2.4% | +5.1% |
| Stealing / 1024 / 4, mixed | 191.43 | 194.78 | +1.7% | 181.95 | -1.7% | -4.4% |

Absolute client cost (thousands of process cycles/roundtrip) and p99 (milliseconds):

| Scheduler / connections / workers | Cycles W1 | Cycles W2 | Cycles Asio | p99 W1 | p99 W2 | p99 Asio |
|---|---:|---:|---:|---:|---:|---:|
| Affine / 64 / 4 | 84.4 | 84.3 | 86.5 | 0.489 | 0.493 | 0.486 |
| Affine / 1024 / 1 | 48.0 | 50.9 | 54.9 | 19.050 | 18.417 | 18.811 |
| Affine / 1024 / 4 | 88.2 | 89.4 | 90.3 | 6.561 | 6.579 | 6.593 |
| Affine / 1024 / 8 | 98.8 | 98.9 | 103.0 | 6.561 | 6.439 | 6.427 |
| Affine / 256 / 4, 64 KiB | 393.7 | 396.1 | 402.0 | 24.879 | 24.616 | 24.519 |
| Affine / 1024 / 4, mixed | 89.4 | 88.1 | 89.4 | 6.584 | 6.529 | 6.628 |
| Stealing / 64 / 4 | 87.9 | 87.6 | 93.4 | 0.571 | 0.572 | 0.429 |
| Stealing / 1024 / 1 | 48.2 | 52.6 | 54.6 | 18.696 | 18.842 | 19.412 |
| Stealing / 1024 / 4 | 88.9 | 88.7 | 90.5 | 6.484 | 6.386 | 6.640 |
| Stealing / 1024 / 8 | 136.4 | 139.9 | 124.3 | 6.724 | 7.296 | 6.959 |
| Stealing / 256 / 4, 64 KiB | 610.6 | 625.5 | 444.1 | 23.797 | 25.021 | 24.765 |
| Stealing / 1024 / 4, mixed | 90.2 | 88.7 | 95.1 | 6.754 | 6.457 | 6.720 |

## Independent Confirmation

Same units and seven-repeat medians, not a replacement for the primary set.
Both sets contribute to the gate.

| Scheduler / connections / workers | Weave 1 | Weave 2 | Change | Asio | Cycles/op change | p99 change |
|---|---:|---:|---:|---:|---:|---:|
| Affine / 64 / 4 | 195.86 | 197.65 | +0.9% | 199.37 | -0.1% | +1.8% |
| Affine / 1024 / 1 | 95.79 | 92.25 | -3.7% | 82.52 | +3.9% | +24.6% |
| Affine / 1024 / 4 | 189.18 | 188.81 | -0.2% | 190.43 | +0.5% | +0.2% |
| Affine / 1024 / 8 | 179.62 | 178.69 | -0.5% | 178.15 | +0.8% | +2.7% |
| Affine / 256 / 4, 64 KiB | 11.06 | 11.48 | +3.8% | 11.37 | -2.6% | -1.3% |
| Affine / 1024 / 4, mixed | 191.29 | 191.08 | -0.1% | 190.61 | -0.5% | -0.2% |
| Stealing / 64 / 4 | 196.91 | 199.73 | +1.4% | 188.46 | -1.6% | -2.2% |
| Stealing / 1024 / 1 | 80.40 | 88.36 | +9.9% | 80.91 | -8.6% | +3.9% |
| Stealing / 1024 / 4 | 192.92 | 194.51 | +0.8% | 191.93 | -1.5% | -1.0% |
| Stealing / 1024 / 8 | 184.07 | 184.32 | +0.1% | 171.83 | +2.0% | +3.7% |
| Stealing / 256 / 4, 64 KiB | 11.37 | 11.32 | -0.5% | 10.76 | -2.8% | +0.4% |
| Stealing / 1024 / 4, mixed | 194.88 | 194.43 | -0.2% | 190.28 | +0.5% | +0.6% |

## Variability

| Set | Maximum Weave throughput CV | Maximum cycles/op CV | Maximum p99 CV | Minimum progress per connection |
|---|---:|---:|---:|---:|
| Primary | 27.8% | 17.5% | 190.8% | 7 |
| Confirmation | 21.8% | 18.3% | 178.2% | 6 |

Outliers were retained. Primary Weave 2 affine 64 KiB repetition 3 had p99
171.991 ms. Primary Weave 1 mixed traffic also had a 90.240 ms p99 trial.
Confirmation's highest p99 was 119.170 ms in Asio shared 64 KiB traffic, and
Weave 2 affine 1024/four-worker traffic had a 66.279 ms p99 trial. Causes are not
established. The maximum observed RTT across all trials was 192.497 ms.

Short CI windows do not establish the absence of rare stalls. For example, the
single-worker stealing throughput median changed -8.5% in primary but +9.9% in
confirmation; its primary 90% interval was [-26.5%, +2.8%]. That is inconclusive,
not proof of a regression or improvement. The gate correctly refused to pass.
See [gate.json](gate.json) for every interval and decision, and [analysis.json](analysis.json)
for absolute CPU-time/core-equivalent diagnostics and per-implementation CVs.

## Validation and Reproduction

- Full Debug and Release CTest: **8/8 passed each**.
- Native Task/affinity suite: **20 cases, 85,568 assertions passed**.
- **13 classifier/input-validation tests**, including noisy data not passing,
  invalid counters, duplicate repetitions, incomplete matrices, and OS-default placement.
- Supervisor tests passed for exit-code forwarding, logs, timeout, and child-tree
  cleanup; path-with-spaces handling is covered.
- Real runner timeout smoke tests, with and without isolation, returned exit 4
  in approximately 2.1 seconds under a five-second test budget (three seconds
  reserved for cleanup). The full run returned exit 2 in 252.8 seconds.

```powershell
powershell -NoProfile -File weave2/run-concurrent-gate.ps1 -IsolateCpus -OutputDirectory weave2/results/2026-10-04-ci-gate-5m-run2
```

Use a fresh output directory on reproduction. Omit `-IsolateCpus` on CI machines
without 12 available physical cores. The default budget still cannot exceed
300 seconds. Build/test steps are separate from the timed benchmark invocation.

Evidence: [primary](comparison.json), [confirmation](confirmation.json),
[environment/source hashes](environment.json), [analysis](analysis.json),
[gate decision](gate.json), [wall-clock record](run.json), [worker log](run.log).

The [first setup attempt](../2026-10-04-ci-gate-5m/run.log) failed before any trial:
PowerShell had unwrapped the single affinity argument into a scalar, which native
splatting split into characters. Typing that argument list as `string[]` fixed
the launcher. Its invalid evidence was preserved, not mixed into these results.
This second attempt is the only complete performance run of the five-minute
profile; no statistical failure triggered a repeat.
