# 1,024-connection benchmark

Windows 11, Ryzen 9 9900X (12 cores / 24 logical processors), MSVC 19.44
x64 Release. Asio 1.36.0 and Google Benchmark 1.9.4 are pinned in CMake.
Coroutine frame recycling is enabled for both clients; profiling and ASan are
disabled for timing. The host is not isolated and threads are not CPU-pinned.

## Workload

- 1,024 simultaneously established persistent TCP connections, with TCP_NODELAY.
- 1, 4, or 8 client workers; Weave affinity/stealing and Asio shared/sharded.
- The identical Asio peer uses four single-threaded shards, not 1,024 threads.
- Each timed batch spawns 1,024 tasks, each performing 32 successive 1 KiB
  roundtrips, then joins them. Tasks progress independently between batch joins.
- Every response is compared with its connection-specific payload. Both clients
  use preallocated buffers, complete writes, and exact reads.
- Connection setup, acceptance checks, one warmup exchange per connection,
  thread startup, and teardown are excluded. Task submission/joins are included.
- Time is milliseconds per 32,768-exchange batch; throughput counts aggregate
  roundtrips. Byte throughput counts both directions (64 MiB per batch).

This measures client-to-peer loopback throughput, not server acceptance rate,
remote-client capacity, request latency percentiles, or pure runtime overhead.
The shared peer and Windows TCP stack can limit scaling. A batch time divided by
32,768 is NOT the latency experienced by an individual concurrent request.

There is no pre-change 1,024-connection workload, so its Weave before/after and
percentage change are N/A. The old 32-connection workload is retained as a
separate before/after control, not an interchangeable baseline.

## Results

Final 1,024-connection results: medians of five repetitions, 1 s minimum per
repetition. Each cell is **milliseconds per batch (real-time CV)**. Lower is
better. All 60 individual samples succeeded; no samples were discarded.

| Client workers | Weave affine | Weave stealing | Asio shared | Asio sharded |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 338.03 (11.13%) | 333.28 (2.87%) | 375.21 (11.76%) | 421.99 (13.40%) |
| 4 | 173.18 (6.33%) | 163.78 (0.40%) | 173.25 (3.44%) | 172.97 (2.48%) |
| 8 | 180.51 (0.75%) | 174.32 (0.69%) | 188.47 (1.90%) | 180.50 (4.92%) |

Aggregate roundtrips/sec at the median:

| Client workers | Weave affine | Weave stealing | Asio shared | Asio sharded |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 96,939 | 98,319 | 87,333 | 77,651 |
| 4 | 189,212 | 200,072 | 189,133 | 189,441 |
| 8 | 181,533 | 187,975 | 173,865 | 181,544 |

Four-worker stealing has the lowest median here. Eight client workers do not
improve throughput over four in this fixture. That does not establish a general
runtime scaling limit: the four peer threads and local TCP stack are also in the
measurement. Single-worker Asio shared/sharded have the same context/thread
topology, yet differ materially in this run, with 11.76-13.40% CV; treat that
single-worker gap as variability, not a sharding result. Weave affine at one
worker also has an outlier. These are workload-specific observations, not a
blanket performance claim or a stable regression gate.

## 32-Connection Control

Eight client workers, 32 connections, one 1 KiB exchange per connection per
batch, the original blocking-peer fixture. These are medians of seven
repetitions at 0.5 s minimum, in **microseconds per batch**. All 28 samples in
each file succeeded. Positive changes are slower.

| Implementation | Before us | After us | Change | Before CV | After CV |
| --- | ---: | ---: | ---: | ---: | ---: |
| Weave affine | 172.72 | 172.84 | +0.07% | 1.76% | 0.97% |
| Weave stealing | 186.37 | 186.99 | +0.33% | 1.36% | 0.68% |
| Asio shared | 178.23 | 179.14 | +0.51% | 1.93% | 0.74% |
| Asio sharded | 177.80 | 179.02 | +0.69% | 2.67% | 0.61% |

All median changes are smaller than observed variability. No meaningful change
is demonstrated in this control. This is not a full-suite performance-regression
claim; the full suite was exercised for correctness, not rerun for long timings.

## Recorded Runs

All runs use randomized repetition interleaving. Builds/tests do not overlap
performance runs. The first exploratory run included a TCP-state diagnostic
query and must not be treated as a quiet measurement.

| File | Cases | Repetitions | Minimum time | Role |
| --- | ---: | ---: | ---: | --- |
| control-before-aggregates.json | 4 | 5 | 0.5 s | Initial old-workload summary only |
| connections-initial-aggregates.json | 12 | 5 | 0.5 s | Invalid exploratory evidence; aggregate-only output can hide failed samples |
| control-after-aggregates.json | 4 | 5 | 0.5 s | Initial old-workload summary, substantial noise |
| control-before.json | 4 | 7 | 0.5 s | Raw pre-change control samples |
| connections-diagnostic.json | 12 | 5 | 0.001 s | Pre-fix diagnostic, four connection-setup failures; not performance evidence |
| connections.json | 12 | 5 | 1 s | Final workload, individual samples retained |
| control-after.json | 4 | 7 | 0.5 s | Raw post-change control samples |

The first fixture closed all clients before stopping the peer. Repeated setup
eventually failed, consistent with client ephemeral-port exhaustion from
TIME_WAIT accumulation. The final harness stops and drains the peer first, only
after every client exchange has joined and validated its payload, then destroys
client sockets on their owning workers. No system TCP settings were changed.
Windows' port-exhaustion behavior is described in
[Microsoft's troubleshooting guide](https://learn.microsoft.com/en-us/troubleshoot/windows-client/networking/tcp-ip-port-exhaustion-troubleshooting).

The aggregate-only exploratory files cannot establish that all repetitions
succeeded: Google Benchmark omits failed samples when computing aggregates.
They are retained for transparency, not used for comparisons. The diagnostic
run records failures directly. Final runs retain and check every raw sample.

## Reproduction

```powershell
powershell -ExecutionPolicy Bypass -File scripts/bench.ps1 -Filter ManyConnections -Seconds 1 -Repetitions 5
```

For the unchanged control workload, select:

```text
^(WeaveMulticoreTcp/8/|WeaveStealingTcp/8/|AsioMulticoreTcp<.*>/8/)
```

Do not pass `--benchmark_report_aggregates_only=true`: it removes the raw
samples needed to validate errors. The normal benchmark script does not use it.

## Verification

Debug and Release CTest pass all three targets, including all 68 benchmark
cases. The five new fixture tests cover fragmented persistent echoes, EOF,
invalid initialization, unexpected resets, pending read/accept cancellation,
and repeated server-first teardown. They also pass 20 consecutive ASan runs.
No Weave runtime or public API code changed in this benchmark addition.
