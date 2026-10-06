# Benchmark methodology

For the local Windows Weave/Asio/Tokio core-count sweep and published README results,
see [runtime scaling](runtime-scaling.md). All measurement suites are manual opt-ins;
correctness CI does not run them. Raw evidence is uploaded separately from source.

The suite compares Weave and standalone Asio 1.36.0 with C++ coroutines,
libuv 1.53.0 with its native TCP callbacks, and uSockets at `7a7c820` with
native callbacks over its libuv backend. All clients disable exceptions.
Google Benchmark 1.9.4 drives all four. Dependencies are pinned by full commit
in the top-level and benchmarks/CMakeLists.txt files; only benchmark builds
download/link libuv and uSockets.

## Source ownership

| Location | Responsibility |
| --- | --- |
| `modules/core/benchmarks/` | Coroutine calls and fallible Task chains |
| `modules/runtime/benchmarks/` | Balanced/skewed CPU scheduling, including native baselines |
| `modules/tcp/benchmarks/` | Echo latency, bulk/concurrent transfers, multicore TCP and 1,024 connections |
| `benchmarks/support/` | Shared runner, profiling and Asio pool helpers |
| `benchmarks/integration/` | Whole-system throughput, CPU and tail-latency gate |
| `benchmarks/results/` | Local result artifacts, ignored by Git |

Each module declares its own benchmark sources. The root harness combines them
into the same `weave_bench` executable using object libraries so registrations
cannot be discarded as unreferenced archive members. The full development preset
retains all 156 cases and the separate `weave_concurrent` program's original 36 cases.
The IOCP layout comparison adds 60 cases without changing the original gate matrix.
Existing executable paths, filters, workload parameters and measurement loops are unchanged.

## Shared versus sharded IOCP

```sh
cmake --build --preset release --target weave_concurrent --parallel 4
python scripts/bench_iocp.py --isolate-cpus
```

This manual-only experiment compares both Weave layouts under both schedulers,
plus pinned Asio sharded/shared baselines. Ten workloads cover 1/64/256/1024
connections, 1/2/4/8 workers, large transfers, uniform CPU work and uneven CPU
work (every eighth connection is expensive). All sockets remain active; this is
not an idle-client or slow-reader simulation. Connection setup/teardown, warmup,
payload validation, CPU accounting and latency collection match across libraries.

Seven randomized 250 ms windows per case fit a five-minute total budget, with
owned-process/descendant cleanup on timeout. The collector requires every case,
all seven distinct repetitions, at least 1000 RTT samples and progress from every
connection. JSON retains raw evidence, source/binary hashes, medians, CVs, CPU per
operation, p50/p99/p99.9 and exploratory 90% bootstrap intervals. These short,
closed-loop measurements are diagnostic, not proof of a universal performance
advantage or a default-promotion gate. This IOCP-layout experiment does not run in automatic CI.

GetProcessTimes CPU seconds can be coarse/noisy in these short windows, including
zero medians. Do not interpret those as zero CPU cost; compare recorded process
cycles per RTT and retain the CPU-time diagnostics. Cycles are not CPU seconds.
An earlier shared IOCP campaign completed in 227 seconds and did not justify
changing the sharded default. Its raw artifacts are not part of the source checkout.

Narrow builds select the applicable groups. For example:

```sh
cmake -S . -B build/tcp-bench -DWEAVE_MODULES=tcp -DWEAVE_BUILD_BENCHMARKS=ON
cmake --build build/tcp-bench --config Release
```

This includes TCP and core comparisons without linking the Weave runtime.
Select `runtime` for scheduler/core comparisons without Weave TCP.
Select `io` for just the existing core coroutine comparisons: their runner uses
Context, so a core-only comparison build requires IO even though the core library
and correctness tests do not. Native dependencies are not fetched for that IO-only
benchmark build. Multicore Weave TCP cases and the whole-system gate require both
`tcp` and `runtime`; native TCP baselines can use their own workers independently.

## Serial echo

Each serial echo case uses one persistent TCP connection to the SAME blocking echo-peer
implementation on a separate thread. All clients enable TCP_NODELAY, use
preallocated payload buffers, write all bytes, and read exactly the same number.
Setup, connection establishment, and teardown are outside the timed state loop.
Each runtime is driven once for the entire loop, not restarted per message.
Asio and libuv TCP use Windows IOCP; Weave uses GetQueuedCompletionStatusEx.
uSockets uses libuv's socket readiness polling on Windows, with SSL disabled.

Payload sizes: 64 B, 1 KiB, and 64 KiB. One operation pair is outstanding per
case. Elapsed time is per roundtrip; bytes/sec counts BOTH directions. This is
serial loopback request/response latency, not maximum network throughput, a
many-client scalability test, or an HTTP benchmark. Peer scheduling and the
Windows TCP stack may dominate small implementation differences.

The payload is checked after each benchmark case. Peer failures and operation
errors invalidate the case. Tests perform additional content validation across
fragmented transfers and larger payloads. No timed buffer allocation is required
by the harness, but library coroutine frames and internal requests may allocate
during measurement. See the native-callback details below before attributing
end-to-end differences solely to an OS backend.

## Additional workloads

| Families | Parameters | What one iteration measures |
| --- | --- | --- |
| Weave / Asio / Libuv / Usockets | 64 B, 1 KiB, 64 KiB | One serial echo roundtrip (original baseline) |
| WeaveCoroutine / AsioCoroutine | No sockets | One nested, immediately completing coroutine call |
| WeaveTask / WeaveReturnTask / WeaveExplicitTask / AsioTask | Depth 1/8/64; success/immediate failure/delayed failure | One fallible chain, including result observation and destruction |
| *Bulk | 1 MiB, 8 MiB | Concurrent send and receive of a complete echo payload |
| *Concurrent | 8 or 32 connections, 1 KiB each | One joined batch of roundtrips across all connections |

The `*` families include all four libraries. The coroutine-only benchmark is
intentionally N/A for the callback libraries: a callback dispatch is not an
equivalent nested coroutine call.

Coroutine tests keep the runtime alive around the timed loop and consume the
returned value. They include whatever frame allocation/reuse and resumption the
compiler/library implements. They do not measure an OS wakeup or claim a fixed
allocation count. The child functions are noinline, but other optimization is
allowed equally for both libraries.

Bulk tests run send and receive simultaneously so a large payload cannot deadlock
against a peer blocked on sending echo bytes back. Reported bytes/sec counts both
directions, not one-way throughput. Buffers and the connection are reused across
iterations. One join per transfer is included in timing.

Concurrent tests use one client runtime thread and one identical blocking peer
thread per connection. Connections are established before timing. Each iteration
starts and joins one exchange per connection. Weave uses when_all; Asio uses its
flat experimental parallel_group with wait_for_all. Child errors are values, not
exceptions. Group creation/join overhead is deliberately included. Time is per
BATCH, items/sec is aggregate roundtrips/sec. Do not compare that time directly
with single-connection latency. Barrier synchronization and peer-thread scheduling
are part of this workload; it is not a saturated independent-client server test.

All cases check final payloads/results and peer status. CTest runs every case with
a short duration as benchmark_smoke, and reported benchmark errors cause a nonzero
process exit. Smoke timings are not performance evidence.

## Multicore workloads

Multicore cases use 1, 2, 4, or 8 background runtime threads and one synchronous
controller thread. Time is per batch of 32 spawned tasks, including task creation,
cross-thread submission, and result joins. Workers are created before timing and
joined afterward. Weave consumes JoinHandles with get(); Asio uses co_spawn with
use_future and consumes std::futures. Native callback clients use a benchmark
submission queue, uv_async_send wakeups, and std::promise/std::future joins.
Future/result-control overhead is included for every library. Operational
networking errors are still values, not exceptions.

WeaveMulticoreCpu and AsioMulticoreCpu execute the same noinline integer-work
function (100,000 dependent update steps per task). The fixed work and result
checks are identical. AsioMulticoreCpu uses one io_context serviced by N threads;
AsioAffineCpu uses N single-threaded contexts. LibuvMulticoreCpu and
UsocketsMulticoreCpu use N affine loops. This is a finite CPU-throughput/scheduling
workload, not a latency or preemption test or a measurement of libuv's thread pool.

WeaveMulticoreTcp and AsioMulticoreTcp use 32 persistent connections, 1 KiB per
echo, and the same blocking peer per connection. Connection setup, buffer setup,
worker startup, and socket teardown are outside timing. Each batch submits one
task per connection and joins all results; bytes/sec counts both directions.
The <false> Asio variant uses one shared io_context with N worker threads. The
<true> variant uses N independent contexts with one thread each and the same
connection-to-worker mapping as Weave. Report both: comparing only against the
shared-context version would hide the architectural sharding tradeoff.
LibuvMulticoreTcp and UsocketsMulticoreTcp use the same affine connection mapping.

These new batches are NOT directly comparable with WeaveConcurrent/AsioConcurrent:
those run a single outer coroutine and join locally, without an external controller
spawning each request. Multicore benchmark CPU-time counters cover the controller,
not all workers; use real_time. Neither the 32 peer threads nor the Windows TCP
stack are free, and localhost results are not a remote-server scalability claim.
Automatic accepted-connection distribution and server accept-rate scaling are
not exercised: each client connection is explicitly created on its target worker.

WeaveStealingCpu and WeaveStealingTcp run the same balanced workloads with
Scheduler::work_stealing. TCP setup/cleanup still uses pinned tasks; exchanges
use movable spawn tasks on the same persistent connections. The original
WeaveMulticore families remain worker-affine for before/after comparisons.

WeaveAffineSkew, WeaveStealingSkew, and AsioMulticoreSkew use 32 jobs with a
deliberately uneven cost: every Nth job does 16 integer-work calls, the others
one, where N is the worker count. Round-robin assignment concentrates the heavy
jobs on one affine worker. This is an imbalance stress case, not a representative
request distribution. Compare schedulers at the SAME worker count: total work
changes with N, so do not interpret cross-row ratios as core-scaling speedups.
AsioMulticoreSkew uses its shared context. AsioAffineSkew, LibuvAffineSkew, and
UsocketsAffineSkew provide the matched affine topology. All execute and validate
identical work at a given worker count. Do not credit a library for a scheduling
advantage without identifying the topology being compared.

## 1,024-connection workload

WeaveManyConnections, WeaveStealingManyConnections, AsioManyConnections,
LibuvManyConnections, and UsocketsManyConnections
keep 1,024 TCP connections open simultaneously, with 1, 4, or 8 client workers.
The Weave variants select worker affinity and work stealing, respectively.
Asio's <false> variant uses a shared context; <true> uses sharded contexts.
The callback libraries use one loop per worker with fixed connection affinity.

All six configurations use the SAME asynchronous Asio echo-peer implementation, with
four fixed threads and four listeners (one per shard). Connections are assigned
round-robin to the listeners. This avoids the 1,024 blocking peer threads that
scaling the original fixture would create. Peer coroutines handle partial reads
and writes, enable TCP_NODELAY, and treat unexpected socket errors as failures.
The peer is benchmark infrastructure, not part of the Weave runtime.

One timed batch spawns one task per connection. Each task performs 32 successive
1 KiB write-all/read-exactly roundtrips, validating every response against its
connection-specific payload. Connections progress independently until the batch
join: there is no barrier between individual roundtrips. That is 32,768 exchanges
and 64 MiB counting both directions per batch. Time is in milliseconds per BATCH;
items/sec is aggregate roundtrips/sec, not per-connection throughput or latency.
Counters record connection count, client workers, peer workers, and rounds.

Setup, acceptance of all 1,024 connections, one warmup exchange per connection,
buffer allocation, and teardown are outside timing. Spawn/join costs are included,
amortized over 32 exchanges. Sockets and buffers persist across batches. Weave
creates and destroys sockets on their owning workers; only the stealing variant
allows the exchange tasks to migrate. All libraries use exception-free I/O.
After all exchanges have joined, the peer is stopped before closing client sockets.
This keeps repeated setup from accumulating active-close TIME_WAIT on thousands
of client ephemeral ports. No system TCP settings are changed.

This is an end-to-end loopback client-load test, not a Weave server accept-rate
test, a pure scheduler benchmark, or a tail-latency measurement. The fixed peer
threads and Windows TCP stack can limit throughput. More client workers need not
improve performance once another resource is saturated. Do not compare its batch
time directly to the older 32-connection, one-roundtrip workloads. CPU-time
counters still cover the controller, not all workers; compare real_time.

The benchmark_fixtures CTest target checks fragmented persistent exchanges, EOF,
unexpected connection resets, and shutdown with pending reads/accepts. The full
benchmark_smoke suite now covers 156 cases, including 44 native-callback cases,
eight additional sharded Asio CPU cases, and 36 fallible-chain cases. The explicit
Task reference uses the current Task implementation with manual `as_result`
checks; it is not a retained version of the former coroutine implementation.

## Native callback baselines

`test_support/callback_clients.hpp` uses uv_tcp/uv_read_start/uv_try_write/uv_write for
libuv and us_socket_context_connect/us_socket_write/on_data/on_writable for
uSockets. Neither adapter routes client traffic through Weave or Asio. Buffers,
callback state, and libuv request storage are reused. Writes are bounded to
64 KiB, handle partial progress and backpressure, and retain storage through
completion or close. Every exchange waits for all sent and echoed bytes.

Callbacks receive continuously, including while writes are pending. This is the
native API style, not a coroutine emulation. Serial and concurrent cases still
have just one logical request/response per connection; bulk deliberately permits
full-duplex progress in all libraries. libuv reads into application receive
storage; uSockets delivers a library-owned buffer which the adapter copies to
the preallocated receive buffer. These are end-to-end API comparisons, not
identical instruction paths or isolated kernel-call measurements.

Single-thread cases run callbacks directly on the main loop and use a countdown
for joined batches, without cross-thread futures. Multicore cases assign
connections round-robin to fixed loops. One external submission and one future
per connection/batch are included in timing, matching the coroutine clients'
controller/worker boundary. The benchmark-only intrusive command queue uses a
mutex and uv_async_send; coalesced wakeups never discard commands. This is not a
libuv/uSockets general-purpose task runtime or a work-stealing implementation.
CPU results include this submission adapter, not just library internals.

Connection establishment, context creation, socket options, and payload setup
are untimed. The uSockets adapter checks SO_ERROR and getpeername in on_open:
the pinned Windows backend can report writable for a refused connection. This
keeps refusal a failed setup rather than a misleading successful benchmark.
Upstream sources are unmodified. Plain TCP is built, with no TLS submodules.

Additional fixture tests cover refused/pending connects, tiny send buffers,
fragmented and repeated transfers, corrupt responses, early EOF, pending-write
cancellation, concurrent batches, and shutdown with in-flight callbacks. ASan
benchmark builds instrument both native libraries as well as the adapters.

Upstream references: [libuv loop/threading design](https://docs.libuv.org/en/v1.x/design.html),
[libuv stream and request lifetimes](https://docs.libuv.org/en/v1.x/stream.html),
and the [pinned uSockets libuv backend](https://github.com/uNetworking/uSockets/blob/7a7c820db4740c2a2a4faf0d918c4eec2ac1fac3/src/eventing/libuv.c).

## Running

Run `python scripts/bench.py` on a quiet machine, preferably with a stable power plan.
The script uses randomized case interleaving, five repetitions, and 0.5 seconds
minimum per repetition. It records machine details, dirty worktree status, Git
revision, and raw Google Benchmark JSON. Keep raw data when reporting results.
Do not use aggregate-only reporting for evidence: Google Benchmark can omit
failed repetitions from aggregates. Check individual JSON samples for errors.

```sh
# All cases, including all four libraries' networking workloads.
python scripts/bench.py
# Select one kind of workload with a Google Benchmark regex.
python scripts/bench.py --filter "Bulk" --seconds 1 --repetitions 5
# 1,024 connections, both Weave modes, shared/sharded Asio, libuv, and uSockets.
python scripts/bench.py --filter "ManyConnections" --seconds 0.5 --repetitions 5
# Only native callback baselines.
python scripts/bench.py --filter "Libuv|Usockets" --seconds 0.5 --repetitions 5
```

The separate `weave_concurrent` executable records every RTT for per-window p99,
process CPU seconds, and client cycles/roundtrip. Its [five-minute gate](gate.md)
includes both schedulers, same-code controls, and a saved-artifact baseline option
for version-to-version comparisons. See [concurrent methodology](concurrent.md).

Still missing: connection churn, explicit slow-reader/backpressure scenarios,
Asio allocation instrumentation, and remote, independently paced client load.
Neither repetition variance nor the batch timings above provide p99 latency.

## Instrumentation

Configure a separate build with `-DWEAVE_PROFILE_RUNTIME=ON` to record Weave
coroutine frame allocation requests, heap misses, cache hits, requested/heap
bytes, read/write submissions, immediate successes, completion packets,
dequeues, and fairness posts. It also times socket submission calls and
completion dequeue calls (including blocking) using steady_clock. These are
wall-clock regions, NOT sampled CPU attribution. Counters
are reported by the coroutine, bulk, and concurrent cases. Root/setup overhead
inside the measured wrapper is amortized into the per-iteration counters.

Instrumentation adds counter updates and clock reads. Use it to
explain work performed, NOT to compare timing against an uninstrumented Asio.
Use the default Release build for timing. Compiler-elided allocations will not
call the allocation hook, and the hook counts coroutine frames only, not all
process allocations. `frames/iteration` counts logical allocation requests;
`frame_heap_allocs/iteration` counts actual heap misses. Warm caches can produce
zero misses without eliminating frame creation/destruction. Google Benchmark's
calibration calls may warm the cache before measured repetitions.

Compare a separate `-DWEAVE_RECYCLE_FRAMES=OFF` build with the normal build to
isolate recycling. Keep normal Asio recycling enabled for performance comparisons.
`ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING` is useful only as a diagnostic control,
not as a weaker baseline to claim a win against. Benchmark JSON records the
Weave and Asio recycling settings and whether runtime profiling is enabled.

Do not infer application tail latency from repetition statistics. Do not compare
Debug against Release, different payloads, or different connection concurrency.
Windows thread CPU-time resolution can make short-case CPU counters noisy or
zero; use real_time for this suite. Repeat close results before drawing conclusions.

## Result artifacts

`benchmarks/results/` is ignored local output, not part of the source checkout.
Keep raw samples, logs, environment metadata, and analysis together for each run;
do not commit them. Preserve existing evidence when rerunning or reanalyzing.

Curated reports may live in documentation, but published performance claims should
link to separately available raw artifacts, including source/binary hashes and
environment details. A favorable summary table alone is not sufficient evidence.
The gate accepts an external baseline directory for version-to-version comparisons.
Correctness tests use synthetic fixtures and do not depend on local result archives.
