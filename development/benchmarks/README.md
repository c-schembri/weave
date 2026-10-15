# Benchmarks

Performance suites are **manual opt-ins**, never automatic correctness-CI gates.
Run on a quiet machine with matched Release builds. Keep raw samples, failures,
environment/source/binary hashes and analysis together under ignored
`benchmarks/results/`; publish release assets, not tracked JSON/log directories.

See [runtime scaling and results](../../modules/runtime/benchmarks/README.md) and
[PostgreSQL/libpq results](../../modules/postgres/benchmarks/README.md).

## Suite Layout

| Location | Comparisons |
| --- | --- |
| `modules/core/benchmarks` | Nested calls and fallible Task chains |
| `modules/runtime/benchmarks` | Balanced/skewed multicore scheduling |
| `modules/tcp/benchmarks` | Serial/bulk/concurrent TCP and 1,024 connections |
| `development/benchmarks/integration` | Whole-system throughput, CPU and RTT tails |
| `development/benchmarks/support` | Runner, profiling and native adapters |

Pinned Asio 1.36.0, libuv 1.53.0 and uSockets 7a7c820 compare against Weave.
Google Benchmark 1.9.4 drives `weave_bench`; dependencies are benchmark-only.
uSockets uses its libuv backend on Windows, not an independent IOCP implementation.
Native callback adapters handle partial writes/backpressure directly; they do not
route traffic through Weave/Asio. Coroutine-only comparisons are N/A for callbacks.

## Run

```sh
cmake --preset windows
cmake --build --preset release --target weave_bench --parallel 4
python development/benchmarks/tools/bench.py
python development/benchmarks/tools/bench.py --filter "Bulk|ManyConnections" --seconds 1 --repetitions 5
```

The driver defaults to randomized interleaving, five repetitions and 0.5 seconds
minimum per repetition. Narrow builds use `WEAVE_MODULES=tcp` or runtime;
multicore Weave TCP needs both. Core coroutine benchmarks need IO for Context.

| Workload | Timed operation and boundary |
| --- | --- |
| Serial echo | One persistent connection, 64 B/1 KiB/64 KiB RTT; same blocking peer |
| Bulk | Full-duplex 1/8 MiB echo; read/write overlap to avoid deadlock |
| Concurrent | Joined batch across 8/32 connections; one RTT each |
| Multicore TCP | 32 persistent connections; external spawn/join overhead included |
| Many connections | 1,024 connections, 32 x 1 KiB RTTs each; 32,768 exchanges/batch |
| Balanced CPU | Identical validated integer work |
| Skewed CPU | Deliberately concentrated heavy jobs; compare same worker count |
| Task chains | Depth 1/8/64, success/immediate/delayed failure; no OS wakeup |

Sockets, payload buffers, setup/warmup/teardown are untimed; logical frame/request
allocation and runtime submission are timed. TCP_NODELAY is matched. Bytes/sec
counts **both directions**. Many-connections uses one common asynchronous Asio
peer with four fixed workers, not thousands of blocking threads.

Report both shared/sharded Asio topologies. Native multicore callback clients use
fixed affine loops with benchmark submission/future adapters, not work stealing.
CPU costs include adapters. Callback receive buffers and coroutine frame paths
differ; these compare end-user APIs, not identical instructions or kernel calls.

Use real_time for Google Benchmark multicore cases: controller CPU counters do
not include every worker. Batch timings/repetition variance are not p99 latency.
Check each raw repetition for failures; aggregates can omit failed samples.

## Whole-System Gate

`development/benchmarks/tools/run_concurrent_gate.py` drives `weave_concurrent` under a five-minute supervisor budget.
It measures persistent traffic, process CPU/cycles, per-window RTT percentiles,
both schedulers, same-code controls and optional archived baselines.
It is a focused regression diagnostic, not a universal performance certification.

```sh
cmake --build --preset release --target weave_concurrent --parallel 4
python development/benchmarks/tools/run_concurrent_gate.py --help
python modules/runtime/benchmarks/tools/bench_iocp.py --isolate-cpus
```

The IOCP experiment compares shared/sharded Weave and Asio across ten workloads,
seven randomized 250 ms windows, 1/64/256/1,024 connections and 1/2/4/8 workers.
It requires the complete matrix, distinct repetitions, progress on every connection
and at least 1,000 RTT samples. Evidence includes CV, tails, CPU/cycles and exploratory
90% intervals. Short-window CPU time may be quantized/zero; zero is not free CPU.

Collectors own descendant cleanup on timeout. Missing evidence, invalid payloads,
timing violations and noisy affected metrics cannot be promoted to passing wins.
No adaptive stopping, selective retries or deletion of unfavorable samples.
For promotion claims, use the longer [matched scaling protocol](../../modules/runtime/benchmarks/README.md)
and appropriate hardware; longer windows cannot remove client/kernel ceilings.

## Instrumentation

Separate `WEAVE_PROFILE_RUNTIME=ON` builds record coroutine requests/cache misses,
bytes, socket submissions/immediate completions/dequeues/fairness posts and measured
wall-clock regions. They are explanatory counters, not CPU attribution or timing wins
against an uninstrumented competitor. Elided allocations do not call hooks; cache
hits do not mean no frame creation. Native adapter fixture tests cover fragmentation,
refusal, early EOF, corrupt data, tiny buffers and pending-callback shutdown.

Keep normal Asio recycling enabled. `WEAVE_RECYCLE_FRAMES=OFF` or disabled Asio
recycling is a diagnostic control, not a deliberately weakened comparison.
Runtime trace builds are also diagnostic-only.
[Trace collection](../../modules/runtime/benchmarks/README.md#diagnostic-tracing).

Closed-loop localhost results are not internet throughput, open-loop service
latency, server accept-rate capacity or universal library rankings.
Remote independently paced load, churn and explicit slow-reader campaigns are
separate measurements; these suites do not establish them.
