# Selectable scheduler baseline

Windows 11 build 22631, Ryzen 9 9900X (12 cores, 24 logical processors), MSVC
19.44.35221 x64 Release. Asio 1.36.0 and Google Benchmark 1.9.4 remain pinned.
Both libraries use frame recycling; Weave runtime profiling is disabled.
The observed power plan was Ultimate Performance. This was not an isolated host.
See environment.json for executable hashes, configuration, and the dirty Git base.

## Implementation and scope

RuntimeOptions now selects Scheduler::worker_affine (the unchanged default) or
Scheduler::work_stealing. spawn_on remains pinned in both modes. Stealing uses
per-worker movable/pinned queues and serializes each spawned root's continuations.
Ready work can migrate after yield, I/O, and asynchronous joins, not only before
first execution. Directly awaited and when_all children remain serialized within
their root, so pending operations cannot resume before their submission unwinds.

Socket IOCP registrations remain sharded. Migrating tasks may access their sockets
from workers in the same stealing runtime; they cannot move sockets to another
runtime or the main thread. Socket creation/close/cancellation use an exclusive
registry lock, while submissions take a shared lock. Affine contexts do not take
these locks. Stealing metrics use atomic updates. Queued I/O continuations use
intrusive records, not per-operation heap allocations or shared_ptrs.

This is a mutex-based scheduler baseline, not a lock-free implementation, Tokio
feature parity, automatic connection distribution, or a blocking-work pool.
Per-worker IOCP drivers still need their worker to run. See docs/runtime.md for
the ownership, custom-awaiter, same-socket concurrency, and shutdown contracts.

## Longer mode comparison

Seven repetitions, 0.5s minimum each, randomized case interleaving, from
confirm-after.json. All rows use eight workers and 32 tasks per batch. Lower
elapsed time is better. Stealing change compares the two modes in this binary,
not the pre-change implementation. CPU Asio uses one shared io_context; both
shared and sharded contexts are shown for TCP.

| Workload | Affine | Stealing | Stealing change | Asio shared | Asio sharded |
| --- | ---: | ---: | ---: | ---: | ---: |
| Balanced CPU | 507.351 us | 497.209 us | -2.0% | 510.337 us | n/a |
| Uneven CPU | 7.174 ms | 2.408 ms | -66.4% | 2.374 ms | n/a |
| TCP, 32 x 1 KiB | 169.261 us | 185.465 us | +9.6% | 180.033 us | 177.316 us |

| Workload | Affine CV | Stealing CV | Asio shared CV | Asio sharded CV |
| --- | ---: | ---: | ---: | ---: |
| Balanced CPU | 2.91% | 1.13% | 3.18% | n/a |
| Uneven CPU | 1.28% | 1.88% | 1.03% | n/a |
| TCP | 0.52% | 3.16% | 5.38% | 3.08% |

Stealing is about 3x faster than affinity in this deliberately unfavorable
assignment pattern, but about 10% slower for this balanced TCP workload. The
small balanced-CPU differences do not establish a winner. These results support
making the choice explicit, not making stealing the default or claiming a general
advantage over Asio. Asio's shared ready queue is a load-balancing baseline; it
is not being labeled a work-stealing implementation.

Balanced CPU tasks perform the same 100,000 dependent integer updates as before.
In the uneven case, every Nth task performs 16 such work calls, others one, where
N is the worker count. This concentrates the heavy tasks on one round-robin
affine worker. Work is identical across libraries/modes at a given N, but total
work changes with N: cross-row ratios below are NOT core-scaling speedups.

TCP reuses 32 connections and the same blocking EchoPeer per connection. Setup,
buffers, workers, and teardown are outside timing. Spawn/result-control creation,
submission, and joins are timed. Affine tasks use spawn_on; stealing tasks use
spawn on the same persistent sockets. This is not an accept-rate, remote-server,
saturation, or tail-latency test. Workers plus controller and 32 peer threads
all compete for host resources.

## Full mode sweep

Means from after.json: five repetitions, 0.3s minimum, randomized interleaving.
Earlier noisy measurements are not discarded in favor of the confirmation above.

| Workers | CPU affine | CPU stealing | CPU Asio | Uneven affine | Uneven stealing | Uneven Asio |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 3.660 ms | 3.628 ms | 3.636 ms | 57.255 ms | 57.418 ms | 62.096 ms |
| 2 | 1.862 ms | 1.824 ms | 1.853 ms | 34.401 ms | 16.021 ms | 15.392 ms |
| 4 | 0.926 ms | 0.935 ms | 0.947 ms | 14.346 ms | 4.634 ms | 4.823 ms |
| 8 | 0.496 ms | 0.504 ms | 0.506 ms | 7.151 ms | 2.405 ms | 2.357 ms |

Balanced-CPU CV was 0.64-3.11%. Uneven CPU CV was generally 0.96-2.21%, but
affine/2 had 37.37% CV and Asio/1 had 17.79% CV. Do not infer a large advantage
over Asio from those outliers.

| Workers | TCP affine | TCP stealing | Asio shared | Asio sharded |
| --- | ---: | ---: | ---: | ---: |
| 1 | 315.951 us | 313.762 us | 345.630 us | 350.084 us |
| 2 | 198.734 us | 201.337 us | 209.309 us | 244.297 us |
| 4 | 216.380 us | 239.535 us | 208.931 us | 263.243 us |
| 8 | 172.410 us | 191.590 us | 195.077 us | 179.202 us |

| Workers | Affine CV | Stealing CV | Asio shared CV | Asio sharded CV |
| --- | ---: | ---: | ---: | ---: |
| 1 | 4.83% | 1.75% | 3.56% | 6.56% |
| 2 | 5.28% | 15.41% | 2.84% | 19.96% |
| 4 | 7.69% | 27.56% | 3.95% | 42.47% |
| 8 | 1.82% | 2.64% | 17.86% | 2.27% |

## Before/after regression check

The pre-change executable was saved before edits. The primary comparison uses
after.json followed by before-repeat.json: five repetitions each at 0.3s, with
random case interleaving. Both source versions are dirty worktrees, not clean
release commits. Builds/tests and benchmark executables ran sequentially, never
overlapping each other. Change is (after / before - 1); positive means slower.
Asio here is the matched workload in the after binary, using shared contexts
for multicore workloads. The full raw before controls are retained too.

| Workload | Weave before | Weave after | Change | Asio after |
| --- | ---: | ---: | ---: | ---: |
| Coroutine call | 10.646 ns | 11.604 ns | +9.0% | 16.987 ns |
| Echo 64 B | 17.098 us | 15.262 us | -10.7% | 16.301 us |
| Echo 1 KiB | 16.332 us | 19.987 us | +22.4% | 16.538 us |
| Echo 64 KiB | 30.562 us | 30.268 us | -1.0% | 30.286 us |
| Bulk 1 MiB | 353.608 us | 336.946 us | -4.7% | 609.331 us |
| Bulk 8 MiB | 3.541 ms | 3.617 ms | +2.1% | 3.463 ms |
| Single-worker batch of 8 | 78.594 us | 92.822 us | +18.1% | 88.923 us |
| Single-worker batch of 32 | 296.477 us | 299.108 us | +0.9% | 347.578 us |
| CPU, 1 worker | 3.591 ms | 3.660 ms | +1.9% | 3.636 ms |
| CPU, 2 workers | 1.859 ms | 1.862 ms | +0.1% | 1.853 ms |
| CPU, 4 workers | 0.939 ms | 0.926 ms | -1.4% | 0.947 ms |
| CPU, 8 workers | 0.506 ms | 0.496 ms | -2.0% | 0.506 ms |
| TCP, 1 worker | 302.875 us | 315.951 us | +4.3% | 345.630 us |
| TCP, 2 workers | 200.927 us | 198.734 us | -1.1% | 209.309 us |
| TCP, 4 workers | 207.303 us | 216.380 us | +4.4% | 208.931 us |
| TCP, 8 workers | 170.216 us | 172.410 us | +1.3% | 195.077 us |

All multicore before/after rows use affine mode, not an implicit mode switch.
The benchmark harness was extended to select modes/workloads, but the original
balanced CPU and TCP work, payloads, and connection mappings are unchanged.
New uneven cases have no old-executable baseline.

| Workload | Weave before CV | Weave after CV |
| --- | ---: | ---: |
| Coroutine | 0.60% | 4.88% |
| Echo 64 B | 11.72% | 1.60% |
| Echo 1 KiB | 3.70% | 29.03% |
| Echo 64 KiB | 5.08% | 2.83% |
| Bulk 1 MiB | 10.48% | 18.24% |
| Bulk 8 MiB | 17.35% | 7.41% |
| Batch of 8 | 2.91% | 21.32% |
| Batch of 32 | 1.43% | 3.31% |
| CPU, 1 worker | 1.55% | 2.42% |
| CPU, 2 workers | 3.53% | 1.75% |
| CPU, 4 workers | 1.58% | 0.64% |
| CPU, 8 workers | 2.54% | 1.04% |
| TCP, 1 worker | 1.61% | 4.83% |
| TCP, 2 workers | 14.27% | 5.28% |
| TCP, 4 workers | 4.88% | 7.69% |
| TCP, 8 workers | 3.03% | 1.82% |

Asio's after-run bulk-1-MiB CV was 57%, so its unusually slow mean is not evidence
of a reliable Weave advantage. The apparent echo/batch regressions also need
qualification, not omission. A longer selected confirmation, seven repetitions
at 0.5s (confirm-after.json then confirm-before.json), found:

| Workload | Weave before | Weave after | Change | Asio after |
| --- | ---: | ---: | ---: | ---: |
| Coroutine | 10.858 ns | 11.230 ns | +3.4% | 16.279 ns |
| Echo 1 KiB | 15.909 us | 15.845 us | -0.4% | 15.943 us |
| CPU, 8 workers | 594.972 us | 507.351 us | -14.7% | 510.337 us |
| TCP, 8 workers | 172.695 us | 169.261 us | -2.0% | 180.033 us |

The before CPU/8 confirmation suffered a 41.33% CV outlier; its apparent 14.7%
improvement is NOT a credible speedup. The primary five-repetition CPU comparison
was much steadier. Echo CV was 2.78%/2.98% before/after and did not reproduce the
earlier large regression. Coroutine CV was 0.99%/1.50%; its smaller 3.4% regression
remains visible and unexplained, not declared noise. TCP/8 CV was 1.39%/0.52%.
The noisy batch-of-8 regression was not separately confirmed. No blanket
single-thread no-regression claim is made.

## Raw evidence and reproduction

| File, chronological order | Cases | Repetitions | Minimum seconds |
| --- | ---: | ---: | ---: |
| before.json | 36 pre-change | 3 | 0.2 |
| after.json | 56 post-change | 5 | 0.3 |
| before-repeat.json | 36 pre-change | 5 | 0.3 |
| confirm-after.json | 14 selected post-change | 7 | 0.5 |
| confirm-before.json | 9 selected pre-change | 7 | 0.5 |

All runs use --benchmark_enable_random_interleaving=true and JSON output. No
timed case reported an error. Every exploratory run is retained. The same saved
binary is used for all before runs, and one post-change binary for all after
runs. Exact timestamps, samples, and CVs are in the raw JSON; hashes and flags
are in environment.json. Only real_time is compared: controller-thread CPU time
is not aggregate worker CPU, and repetition CV is not request p99 latency.

```powershell
cmake --build --preset release --parallel
.\build\windows\Release\weave_bench.exe --benchmark_min_time=0.3s --benchmark_repetitions=5 --benchmark_enable_random_interleaving=true --benchmark_out=all.json --benchmark_out_format=json
.\build\windows\Release\weave_bench.exe '--benchmark_filter=((MulticoreCpu|StealingCpu|AffineSkew|StealingSkew|MulticoreSkew|MulticoreTcp|StealingTcp).*/8/|^(Weave|Asio)(/1024/|Coroutine/))' --benchmark_min_time=0.5s --benchmark_repetitions=7 --benchmark_enable_random_interleaving=true --benchmark_out=confirm.json --benchmark_out_format=json
```

## Correctness

Debug and Release CTest pass: 33 doctest cases (existing runtime tests exercised
in both modes) and 56 benchmark smoke cases. Release CTest passed five consecutive
repeat-until-fail runs, including the benchmark smoke test. MSVC ASan passed ten
consecutive runs with 34 test cases, including its allocator-poisoning case.
Both multicore example modes print result 42.

New coverage includes explicit/default/invalid scheduler selection, actual theft
of a burst from a pinned parent, migration after yield while retaining a live
socket and its original Context reference, serialized when_all duplex transfers,
accepted-socket transfer to a different worker, immediate failure, overlapping-I/O
guards, cancellation, EOF, and closed-handle failure. Prior tests cover pending
accept/read shutdown, concurrent producers, dropped handles, nested/standalone
joins, capture reclamation, and stop/submission races in both modes. Both kernel
completion and inline-success paths are tested. ASan does not detect data races;
this is not a ThreadSanitizer result or a proof of production readiness.
