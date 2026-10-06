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
