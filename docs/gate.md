# Five-Minute Performance Gate

Protocol: `weave-task-policy-5m-v1`. The benchmark invocation has a **300-second total
wall-clock budget**, including metadata collection, process startup, warmup,
measurement, draining, and analysis. Build and correctness tests are separate;
automatic CI never runs this gate. The previous hour-long plan was abandoned
before any full run started. The scripts require Python 3.11+, with no pip packages.

```sh
cmake --build --preset release --parallel
python scripts/run_concurrent_gate.py --baseline-directory out/previous --output-directory out/candidate
```

The runner starts a hidden worker suspended, assigns it to a kill-on-close Windows
job, then resumes it. The deadline covers that worker and every descendant,
including benchmark and peer processes. It reserves three seconds for cleanup
and publishing the result. A hang or insufficient time produces a nonzero timeout
result; it never retries, silently extends the budget, or accepts partial evidence.
`run.json` records actual elapsed time and `run.log` preserves worker output.
The process supervisor is tested with a hung worker and a live child process.

## Fixed Work

- Six workloads and both scheduler modes, with the matched sharded/shared Asio
  baselines. No workload coverage was removed.
- One persistent runtime, peer, set of connected sockets, and payload buffers per
  workload/scheduler. Explicit-result and automatic-propagation Tasks use the
  **same fixture**. Both are warmed
  before measurement, including the latency collector. Asio has its own warmed,
  reused fixture and is a descriptive baseline, not the paired comparison.
- Seven paired blocks per case, each with four **250 ms** windows in ABBA/BAAB
  order. Within each block, both labels occupy symmetric positions. Alternate
  the order between blocks to reduce order effects and log-linear drift.
- Each case also runs seven interleaved **A/A blocks**: both labels execute the
  exact same explicit-result Task function. These test whether measurement can distinguish
  identical code from an apparent difference. A/A and A/B share the fixture.
- 756 measured windows: 672 Weave plus 84 Asio, **189 seconds nominal**. Another
  18 seconds warms the fixtures. Setup, draining, and analysis use the remainder.
- Every exchange is validated. Every connection must progress, every trial must
  supply at least 1,000 roundtrips, and the entire planned sequence must complete.

The earlier independent-trial method recreated fixtures for short trials. Its local
run completed in 253 seconds but could not resolve many comparisons. Pairing changes
the experimental design rather than increasing duration or loosening thresholds.
Pairing reduces some sources of variation; it does not make arbitrary machine
noise disappear. All windows are retained, including outliers. Slow CI machines
can time out; failed A/A controls explicitly reject the measurement method/run.
Long research benchmarks remain available through `scripts/bench_concurrent.py` but
are not the gate entry point and are never launched automatically after failure.
The `ci` profile name denotes the bounded protocol, not an automatic workflow.

## Acceptance

There is only one Task implementation. The paired policy comparison checks
automatic propagation against explicit `as_result` handling on that same code,
not a retained copy of the former coroutine implementation. It uses these limits:

| Metric | Required bound on candidate / reference |
|---|---|
| Throughput | Lower confidence endpoint >= 0.95 |
| Client cycles/roundtrip | Upper confidence endpoint <= 1.05 |
| Per-window p99 latency | Upper confidence endpoint <= 1.10 |

For each block, the ratio is the geometric mean of its two B windows divided by
the geometric mean of its two A windows. The estimator is the median of seven
block ratios. Intervals are 90% percentile bootstrap intervals from 20,000
deterministic **whole-block** resamples, never RTTs or independent windows.
These estimate typical window behavior, not pooled latency over the whole run.
The Python implementation preserves the original seeded resampling stream and
percentile indices; migration tests compare decisions and interval endpoints
against archived evidence without modifying it.

First, all **36 A/A controls** must demonstrate two-sided equivalence: the whole
interval within [0.95, 1.05] for throughput/cycles and [0.90, 1.10] for p99. An
apparent improvement is just as suspicious as an apparent regression in A/A.
If any control fails, the result is `measurement_unreliable`, not a Task failure
or a parity pass. A/B numbers remain visible but are not accepted as evidence.

Then all **36 A/B checks** must pass the one-sided limits above. An interval
wholly outside the limit is a regression; one crossing the limit is inconclusive.
The checker validates the complete sequence, labels, implementations, metrics,
and progress before recomputing statistics. No retries, outlier removal, or
conditional selection of blocks. Small-sample per-case intervals assume roughly
exchangeable block noise; they are exploratory, not simultaneous family-wise
guarantees. `gate.json` records all ratios, checks, summaries, and provenance.

| Exit | Result |
|---:|---|
| 0 | All checks pass |
| 1 | At least one established regression |
| 2 | No established regression, but at least one inconclusive check |
| 3 | Invalid/incomplete evidence or setup failure |
| 4 | Wall-clock budget exhausted; owned process tree terminated |
| 5 | A/A controls failed; measurement not reliable enough to judge parity |

CPU time remains a separate diagnostic. A cycles-gate pass is not CPU-seconds
certification. This is a closed-loop paired per-window-p99 gate, not a bound on
rare maximum stalls or an open-loop production SLO. Correctness and sanitizer
tests run separately. No gate outcome automatically changes the default API.

## Version-To-Version Checks

Pass `--baseline-directory` to compare native automatic-propagation Tasks against
a completed historical run on the same host. Without it, the result covers the
paired policy comparison only. Do not report a policy-only pass as proof that a
code change has not regressed performance.

The checker reads raw `paired.json` and `environment.json` artifacts. It accepts
this protocol and the archived prototype protocol `weave2-paired-5m-v2`; the latter
is data compatibility, not a second implementation. Both runs must have the full
Release CI matrix, matching duration and CPU placement, successful exchanges,
valid counters, and passing A/A controls. Hardware, OS version/build, and power
scheme must match. Source and executable hashes describe their respective runs,
not identical code. Historical input hashes are preserved in `gate.json`.

For each version, take the geometric mean of the two automatic-Task windows in
each of seven A/B blocks. Compare the medians using 20,000 deterministic
**independent** bootstrap resamples of the two sets of blocks. The same one-sided
limits apply to these 36 additional checks. Runs in separate processes/times are
not paired: this comparison does not cancel cross-run drift, compiler changes,
or background load. Inconclusive history checks cannot pass. Current A/A failure
still makes the overall run measurement_unreliable.

This adds analysis, not more measured windows; the same 300-second watchdog covers
it. Keep baseline artifacts from a trusted run instead of rebuilding deleted code.
For a first run with no suitable history, establish an artifact and label its
scope honestly. Comparing a run to itself is useful in checker tests, not as
performance evidence.

## Placement

OS-default scheduling is supported on ordinary Windows CI runners. Keep hardware
and placement consistent across baseline/candidate. Do not
run builds or other heavy work concurrently with a performance gate.

On a machine with at least 12 available physical cores in one processor group,
`--isolate-cpus` assigns eight client and four peer cores, one logical processor per
physical core. This avoids sharing SMT cores between the owned processes. It
does not reserve those cores against other applications, pin CPU frequency, or
change system power policy. The same placement is applied to every implementation.
The recorded local run uses this option; it is not required for ordinary CI.

```sh
python scripts/run_concurrent_gate.py --isolate-cpus --baseline-directory out/previous-pinned --output-directory out/candidate-pinned
```

Use a new output directory. `--timeout-seconds` can lower the budget (5-300), not
raise it. `python scripts/check_concurrent_gate.py --directory out/candidate`
reanalyzes completed CI-profile evidence without rerunning measurements. Its
`check_gate()` function returns the decision in-process; `publish=False` prevents
writing a new `gate.json` when inspecting archived evidence.
CTest covers injected metric regressions, log-linear drift cancellation, A/A
false differences, malformed inputs, and supervisor timeout cleanup. A native
paired smoke test also injects a deliberately large CPU cost into the candidate
and verifies that the measured cycles/op increase. It validates gross sensitivity,
not 5% detection power. Injection is prohibited in the actual CI profile.
See [measurement details](concurrent.md) for the workload and CPU-counter caveats.
