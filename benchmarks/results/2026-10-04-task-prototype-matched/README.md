# Weave 2 matched comparison, 2026-10-04

> Historical prototype evidence. Labels, commands, and raw hashes describe the
> original code. That source has since been replaced by native `weave::Task`;
> linked methodology documents now describe the current gate.

## Conclusion

The exact fallible-Task syntax works, but this prototype is **not a speed upgrade**
over Weave 1. Successful call chains cost about 21-32% more with the matched
`co_return unexpected` error origin. Loopback TCP is effectively tied within the
observed variability. Do not replace Weave 1 on performance grounds yet.

Failure-heavy chains have a different balance: depth 8 is slightly faster with
the matched origin in both runs, but depths 1 and 64 remain slower. This does not
establish a general error-path advantage. Weave 2's deeper chains are faster than
the measured Asio chains; that is a narrow microbenchmark result, not evidence
of a faster networking engine.

## Method

- Ryzen 9 9900X, 12 cores / 24 logical processors; Windows 11 Pro build 22631.
- MSVC 19.44.35221.0, x64 Release, C++23, exceptions disabled. Both Weaves use
  the existing frame recycler; runtime profiling and ASan are off for timing.
- Pinned Asio revision: `366dfc44640182cb21c1ebf7efb658a6bec13f5a`.
- One executable, same IOCP transport for both Weaves. Original Weave 1/Asio TCP
  benchmark source is compiled unchanged. This measures the coroutine policy,
  not a new socket implementation.
- Two independent randomly interleaved sets, 7 repetitions per case, minimum
  0.25 seconds per repetition. Each set contains 45 cases and 315 raw samples.
  No benchmark reported an error. No build or test suite ran during timing.
- Chains use depths 1, 8, 64 and deterministic error rates 0%, 1%, 100%.
  Every result/error is checked; recursive chain entry points are non-inlined.
  One iteration is one complete chain, not one coroutine/frame.
- TCP uses one connection, a blocking echo peer, identical payloads, and
  sequential write-all/read-exactly roundtrips. Connection setup is outside the
  timed loop. These are not multicore or 1024-connection measurements.

Reproduce from the repository root after the Release build:

```powershell
./weave2/bench.ps1 -OutputDirectory weave2/results/new-run -Seconds 0.25 -Repetitions 7
```

Times below are primary-run wall-clock medians. Lower is better. Delta is
`100 * (Weave2 / Weave1 - 1)`; positive means slower. Confirmation is the same
delta from the second independent set, not an average across sets. Max CV is the
largest wall-clock coefficient of variation across the implementations in the
workload and both sets (all four chain variants for the chain tables).

## Matched error origin

All three originate the leaf's error with `co_return std::unexpected(...)`.
Weave 1/Asio explicitly check Results at every parent; Weave 2 propagates
automatically. The Weave 2 case name is `Weave2ReturnTask`.

| Depth | Errors | Weave 1 ns | Weave 2 ns | Delta | Asio ns | Confirmation | Max CV |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0% | 13.78 | 18.21 | +32.1% | 17.42 | +21.1% | 6.5% |
| 1 | 1% | 13.78 | 18.21 | +32.2% | 17.52 | +24.7% | 5.6% |
| 1 | 100% | 24.10 | 26.68 | +10.7% | 25.90 | +11.4% | 4.5% |
| 8 | 0% | 95.87 | 121.82 | +27.1% | 272.87 | +28.8% | 1.1% |
| 8 | 1% | 95.50 | 122.54 | +28.3% | 271.38 | +27.1% | 2.0% |
| 8 | 100% | 109.10 | 106.20 | -2.7% | 274.16 | -3.1% | 1.4% |
| 64 | 0% | 767.36 | 925.34 | +20.6% | 2498.80 | +22.8% | 1.6% |
| 64 | 1% | 768.06 | 928.29 | +20.9% | 2489.45 | +22.9% | 1.7% |
| 64 | 100% | 783.36 | 824.57 | +5.3% | 2496.46 | +4.0% | 1.3% |

## Awaited error origin

`Weave2Task` instead originates the error with `co_await weave2::fail(...)`.
Its parent propagation is identical. Keeping this variant exposes the cost of
the additional possible suspension point, including its effect on successful
calls. The different compiled function/frame shape means this is not a pure
subtraction of error-routing instructions.

| Depth | Errors | Weave 1 ns | Weave 2 ns | Delta | Asio ns | Confirmation | Max CV |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 0% | 13.78 | 17.95 | +30.3% | 17.42 | +26.2% | 6.5% |
| 1 | 1% | 13.78 | 18.23 | +32.3% | 17.52 | +25.2% | 5.6% |
| 1 | 100% | 24.10 | 29.57 | +22.7% | 25.90 | +25.1% | 4.5% |
| 8 | 0% | 95.87 | 128.18 | +33.7% | 272.87 | +30.1% | 1.1% |
| 8 | 1% | 95.50 | 128.29 | +34.3% | 271.38 | +28.5% | 2.0% |
| 8 | 100% | 109.10 | 112.83 | +3.4% | 274.16 | -1.9% | 1.4% |
| 64 | 0% | 767.36 | 998.11 | +30.1% | 2498.80 | +24.9% | 1.6% |
| 64 | 1% | 768.06 | 1003.58 | +30.7% | 2489.45 | +24.7% | 1.7% |
| 64 | 100% | 783.36 | 897.97 | +14.6% | 2496.46 | +6.6% | 1.3% |

## TCP roundtrip

The Weave 2 loop uses bare `co_await socket.write_all(...)` and
`co_await socket.read_exactly(...)`, with an outer Result boundary.

| Payload | Weave 1 us | Weave 2 us | Delta | Asio us | Confirmation | Max CV |
|---:|---:|---:|---:|---:|---:|---:|
| 64 B | 16.34 | 16.25 | -0.6% | 16.52 | -0.6% | 3.9% |
| 1 KiB | 16.63 | 16.61 | -0.1% | 16.37 | -2.7% | 5.1% |
| 64 KiB | 31.45 | 31.41 | -0.1% | 30.82 | +0.0% | 4.2% |

These differences do not justify claiming a TCP improvement. The one-node chain
also has noticeable run-to-run drift; use the repeated direction and rough size
of its regression rather than treating the last decimal as stable.

## Allocation diagnostics

A separate `WEAVE_PROFILE_RUNTIME=ON` build ran 3 repetitions at 0.05 seconds for
the 0%/100% Weave chain cases. Its timings are **not** performance evidence.
After amortizing the constant root/boundary frames:

| Chain implementation | Frames per node | Requested bytes per node | Cache bucket |
|---|---:|---:|---:|
| Weave 1 | 1 | 160 | 256 B |
| Weave 2, return-origin | 1 | 192 | 256 B |
| Weave 2, await-origin | 1 | 240 | 256 B |

All variants reuse the same size-class bucket. Heap-allocation counts are zero
or near zero after amortization, not an extra allocation on every Task await.
Additional promise/ownership bookkeeping and compiled frame shape are plausible
costs, but this profile does not isolate their individual causal contributions.
Another allocator is not supported as the next fix by these measurements.

## Correctness checks

- Debug and Release full CTest: 5/5 each, including unchanged Weave 1 regressions.
- Prototype suite: 15 cases, 1462 assertions, all passing.
- MSVC ASan Release: prototype tests and 45-case benchmark smoke test each passed
  three consecutive runs. This is not a claim that the entire legacy ASan suite
  was run.
- Coverage includes 20,000-deep immediate/delayed failure, cleanup-before-recovery,
  skipped statements, multiple awaits per expression, move-only values, partial
  TCP transfer, EOF, cancellation with live buffers, and both runtime schedulers.
- Runtime shutdown tests drain pending AcceptEx completions before destruction.
  Join-all waits for siblings; fail-fast cancellation is intentionally absent.

Only Windows/MSVC was tested. Sanitizer success is supporting evidence, not proof
of every lifetime, scheduler, or cancellation composition.

## Evidence

- [comparison.json](comparison.json): primary raw samples and aggregates.
- [confirmation.json](confirmation.json): independent confirmation samples.
- [environment.json](environment.json): hardware, compiler, dirty-worktree state,
  and executable/source SHA256 values. All 19 recorded source/executable hashes
  were checked against the final benchmark build.
- [profile.json](profile.json) and [profile-environment.json](profile-environment.json):
  separate instrumented diagnostics.
- [Initial experiment](../2026-10-04-task-prototype/README.md): retained rather
  than overwritten when the matched-origin variant was added.

The baseline is the current local Weave 1 source, not an assumed pristine HEAD.
No Weave 1 implementation or protected `examples/echo.cpp` edits were made for
this experiment. No results establish production readiness or general throughput
superiority.
