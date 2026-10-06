# Runtime Performance Investigation

The scheduler uses intrusive runnable queues, bounded steals of at most half a
victim's movable queue (up to 16 roots), rotating victim selection, and per-worker
dispatch lifetime guards. Pinned work remains on its owning worker; local queues
alternate pinned/movable work and keep FIFO service. No per-queue-node allocation,
polling timeout, spin loop, or helper thread was added.

A batch transfer releases the victim lock before acquiring its destination lock.
It publishes remaining work and wakes an idle thief before executing its first
root. A sleeping worker rechecks queues under their locks. These rules preserve
progress when a running worker is temporarily blocked or CPU-heavy.

Shared IOCP has an optional internal native-completion dispatch path. An idle
movable root can resume on its collector within the same runtime; pinned roots
require their designated worker. The per-root lock still serializes execution.
Roots already scheduled or executing are queued, never resumed recursively.
Submission and root/frame cleanup always use deferred scheduling. IO batches and
the existing inline-operation budget bound this work. Shared collector accounting
and per-worker guards both protect Context destruction.

Sharded IOCP retains queue-based completions. Resuming directly on one sharded
collector can concentrate work on that port rather than spread it across workers.
The shared layout is still opt-in; benchmarks must justify any default change.

## Measured Outcome

The [2026-10-06 comparison](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261006-182604)
completed all 560 windows and passed timing validation. It replayed the archived
before executable in every repetition, alongside the new default, shared IOCP,
Asio and Tokio. Debug, Release and AddressSanitizer each passed 22 CTests.

Default-layout throughput changes were mostly within measurement uncertainty.
This iteration does **not** establish a substantial general speedup. For 1,024
clients exchanging 1 KiB, the paired changes were -0.5%, -0.4%, -0.2% and +0.5%
at 1/2/4/8 server cores. At eight cores, paired server cycles/RTT fell 1.9%, but
the 90% interval includes no change. The paired p99.9 reduction was 7.9%
with an exploratory 90% interval of 0.7%-30.0%; this is not a universal tail win.
At four cores, p99/p99.9 instead rose 2.5%/2.7%, with intervals including no change.

Shared IOCP versus the **old sharded default** produced the following results for
the same workload. This combines a layout change and runtime changes; it does
not isolate the native-completion fast path from the previous shared implementation.

| Server cores | Throughput change | Server cycles/RTT change | p99 change | p99.9 change |
| ---: | ---: | ---: | ---: | ---: |
| 1 | +0.3% | -0.3% | +0.2% | -0.8% |
| 2 | +5.1% | -2.3% | -4.4% | -6.5% |
| 4 | +12.3% | -4.4% | -8.8% | -8.7% |
| 8 | -18.2% | -14.4% | +20.1% | +13.1% |

These are medians of matched repetition ratios, not ratios of aggregate medians.
Throughput variability for this workload was at most 1.2% CV. Four/eight-core
comparisons hit client limits, and the eight-core shared candidate trades lower
CPU cost for worse throughput and tails. It is **not promoted to the default**.
The full evidence includes all four workloads, all samples, absolute CPU/tail
measurements, variability and confidence intervals, including unfavourable results.
It does not show Weave consistently beating Asio and Tokio. A separate client
machine and matched offered-load latency tests remain necessary for capacity claims.

## Diagnose A Stall

Use Windows Performance Recorder's CPU/wait profiles when the process has the
required profiling privileges. WPR on the current workspace cannot enable its
profiling policy (`0xc5585011`); that is a tracing limitation, not attribution of
the historical stall. Twelve traced baseline windows did not reproduce the
5.8-second Weave timing failure. The failure remains recorded in the original
[diagnostic evidence](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261006-172307).

An independent, opt-in user-mode trace is available without kernel profiling:

```sh
cmake --preset windows-runtime-bench -B build/runtime-trace -DWEAVE_TRACE_RUNTIME=ON
cmake --build build/runtime-trace --config Release --parallel 4
```

Set `WEAVE_TRACE_DIRECTORY` to an existing ignored output directory before
launching a diagnostic server. Each calling thread creates an exclusive
`PID-TID.weavetrace` file, using a memory-mapped 262,144-entry ring. The hot path
records QPC timestamps, objects and payloads without per-event allocation or
formatted logging. Trace setup failure is printed to stderr. Ordinary builds
compile every trace call out; timing collectors reject trace-enabled builds.

Events cover enqueue, stealing, execution, park/wake, native submission,
completion collection, and shared-port direct dispatch. Use samples' recorded
server PIDs to correlate processes. After all writer processes have exited:

```sh
python scripts/trace_runtime.py benchmarks/results/trace-DIRECTORY --output benchmarks/results/trace-DIRECTORY/analysis.json
```

The analyzer pairs queue-to-execution and per-thread submission/execution/wait
intervals. Wrapped buffers retain only recent history; the possibly overwritten
oldest boundary slot is excluded. Process termination can leave unmatched begins.
Neither a long IOCP wait on an idle worker nor absence of a stall in the retained
history establishes a cause. Instrumentation changes execution costs: never use
these timings as benchmark wins, silently erase the original outlier, or claim
the historical stall has been fixed without reproducing and identifying it.

See [local scaling](runtime-scaling.md) for uninstrumented before/after measurements.
