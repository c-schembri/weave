# Initial Task experiment, 2026-10-04

> Historical prototype evidence. Labels, commands, and raw hashes describe the
> original code. That source has since been replaced by native `weave::Task`;
> linked methodology documents now describe the current gate.

Historical evidence from the initial 36-case harness: two randomized sets of
7 repetitions, at least 0.25 seconds per repetition, with 252 raw samples per
set. No benchmark reported an error.

This harness included only the `co_await fail(...)` Weave 2 error-origin variant;
Weave 1 and Asio used `co_return unexpected(...)`. Parent Task propagation was
automatic, but the comparison also included a different leaf error-origin shape.

The harness was subsequently expanded to retain this variant **and** add the
matched `co_return unexpected(...)` variant. Use the [45-case matched report](../2026-10-04-task-prototype-matched/README.md)
for the final comparison. No Task implementation change separates these two
measurement sets; benchmark source/executable hashes differ because of the new
cases. The original data is preserved rather than silently replaced.

Initial primary-run medians, lower is better:

| Workload | Weave 1 | Weave 2 | Delta | Asio | Confirmation delta |
|---|---:|---:|---:|---:|---:|
| Depth 1, success | 13.70 ns | 17.11 ns | +24.9% | 18.55 ns | +25.3% |
| Depth 8, success | 95.59 ns | 124.83 ns | +30.6% | 266.76 ns | +29.7% |
| Depth 64, success | 769.71 ns | 947.70 ns | +23.1% | 2421.48 ns | +20.7% |
| Depth 8, all errors | 107.32 ns | 110.92 ns | +3.4% | 271.13 ns | +0.6% |
| TCP 64 B | 15.34 us | 15.22 us | -0.8% | 15.29 us | +0.2% |
| TCP 1 KiB | 15.75 us | 15.43 us | -2.0% | 15.74 us | -1.6% |
| TCP 64 KiB | 30.07 us | 30.01 us | -0.2% | 29.97 us | +1.0% |

Maximum wall-clock CV across both sets was 8.1% for the chain cases and 10.7%
for TCP. These samples did not demonstrate a TCP win.

[comparison.json](comparison.json), [confirmation.json](confirmation.json), and
[environment.json](environment.json) record the original samples and hashes.
[profile.json](profile.json) is a separate instrumented diagnostic run and its
timings must not be compared with the uninstrumented results.
