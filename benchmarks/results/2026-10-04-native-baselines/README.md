# Native baseline comparison: 2026-10-04

## Takeaways

- No blanket winner on this Windows loopback workload. The longer 1 KiB serial
  followup is effectively tied: all four medians are 16.61-16.88 us.
- The 1,024-connection, four-worker followup is close: Weave affine 182.24 ms,
  Asio sharded 184.72 ms, libuv 184.39 ms, uSockets/libuv 190.22 ms per batch.
  Weave stealing is 178.26 ms and shared Asio 193.28 ms. These are different
  scheduler topologies, not interchangeable labels.
- The callback adapters are materially slower for 8 MiB bulk echo in both runs.
  The followup gives about 10 ms versus about 4.2 ms for Weave/Asio. This is an
  end-to-end result for these adapters and public APIs, not evidence that
  callbacks or readiness are intrinsically slower. The cause is not profiled.
- Bulk and some local/concurrent measurements remain noisy. The initial
  Weave serial before/after increase of 17.10% disappears in the reversed-order
  followup (-0.05%). Do not interpret every harness delta as a runtime regression
  or improvement; no Weave runtime source was changed in this task.

## Conditions

Windows 11 Pro 22631, Ryzen 9 9900X (12 cores / 24 logical processors),
MSVC 19.44.35221 x64 Release, /O2 /Ob2 /DNDEBUG. No exceptions, runtime profiling,
or ASan in timed executables. Both coroutine frame recyclers remain enabled.
Ultimate Performance power plan, no CPU affinity pinning, nonisolated host.

Pinned versions: Asio 1.36.0, Google Benchmark 1.9.4, libuv 1.53.0
(`840404ce8ba7cc0204be52389a6cfff9f2c90fb6`), uSockets
`7a7c820db4740c2a2a4faf0d918c4eec2ac1fac3`.
uSockets uses the upstream **libuv readiness backend on Windows**, with SSL off.
It is not an independent Windows IOCP TCP backend. Native clients use native
callbacks, fixed-affine loops, and a benchmark-only cross-thread submission
queue with future joins. uSockets receives through a library-owned buffer which
is copied into the application buffer; libuv receives into application storage.
These are matched logical workloads and API boundaries, not identical instruction
paths. See the [full methodology](../../../docs/benchmarks.md).

All tables report median wall time; parentheses are coefficient of variation
(sample standard deviation / mean). Lower time is better. Change is
`100 * (after / before - 1)`; positive is slower. Batch timings are not p99
latency. Five repetitions per case, randomized interleaving within each run.

## Longer Followup

Selected serial, bulk, eight-worker 32-connection, and four-worker 1,024-connection
cases were repeated with a **1 s requested minimum per repetition**, five
repetitions. The new executable ran first, then the saved old executable,
reversing the initial run order. These runs do not replace or delete the initial
sweep. Before/after below means executable version, not chronological ordering.

Multicore rows below use affine Weave and sharded Asio, matching the native
clients' fixed-worker topology. Single-connection rows use the main loop.

| Workload | Weave Before | Weave After | Change | Matched Asio | libuv | uSockets/libuv |
| --- | --- | --- | --- | --- | --- | --- |
| 1 KiB serial (us) | 16.82 (2.9%) | 16.81 (5.7%) | -0.05% | 16.88 (3.1%) | 16.61 (5.8%) | 16.69 (3.6%) |
| 8 MiB bulk (ms) | 3.91 (10.3%) | 4.22 (9.8%) | 7.85% | 4.15 (10.8%) | 10.08 (7.1%) | 10.19 (4.8%) |
| 32 connections, 8 workers (us) | 175.27 (2.5%) | 177.68 (3.5%) | 1.38% | 185.87 (3.3%) | 175.06 (4.0%) | 170.09 (1.8%) |
| 1,024 connections, 4 workers (ms) | 180.02 (3.8%) | 182.24 (1.5%) | 1.24% | 184.72 (1.4%) | 184.39 (1.2%) | 190.22 (0.8%) |

For 32 connections, time is per joined batch of 32 one-KiB exchanges. For 1,024
connections, time is per batch of **32,768** one-KiB exchanges: 32 successive
roundtrips per connection, four common peer workers, all connections accepted and
one warmup exchange per connection before timing. Both directions count toward
bytes/sec. Peer implementation, payloads, validation, and timing boundaries match.

All topologies in the longer 32-connection followup, **us/batch**:

| Workers | Weave Affine | Weave Stealing | Asio Shared | Asio Sharded | libuv Affine | uSockets/libuv Affine |
| --- | --- | --- | --- | --- | --- | --- |
| 8 | 177.68 (3.5%) | 184.00 (2.1%) | 182.19 (3.7%) | 185.87 (3.3%) | 175.06 (4.0%) | 170.09 (1.8%) |

All topologies in the longer 1,024-connection followup, **ms/batch**:

| Workers | Weave Affine | Weave Stealing | Asio Shared | Asio Sharded | libuv Affine | uSockets/libuv Affine |
| --- | --- | --- | --- | --- | --- | --- |
| 4 | 182.24 (1.5%) | 178.26 (3.7%) | 193.28 (1.0%) | 184.72 (1.4%) | 184.39 (1.2%) | 190.22 (0.8%) |

The stealing controls changed from 187.44 to 184.00 us (-1.84%) for 32 connections
and 178.65 to 178.26 ms (-0.22%) for 1,024 connections. The bulk control's +7.85%
delta accompanies roughly 10% variability in both versions, while Asio's bulk
control also moves from 3.87 to 4.15 ms. Small differences are not firm speedups.

## Full Sweep

The complete initial sweep requested **0.5 s minimum per repetition**, five
repetitions. Below are all 120 current cases, without selecting the fastest
samples. The old executable ran first (68 cases), followed by the new executable
(120 cases). Each case has exactly five successful raw samples.

### Local Networking

Single client loop on the main thread, persistent connection(s), identical
blocking echo peer per connection. Serial time is per roundtrip; bulk time is per
payload echo; concurrent time is per joined batch. All values below are **us**.

| Workload | Weave | Asio | libuv | uSockets/libuv |
| --- | --- | --- | --- | --- |
| 64 B serial | 17.12 (3.0%) | 16.95 (45.1%) | 16.84 (4.6%) | 17.84 (7.1%) |
| 1 KiB serial | 18.22 (8.0%) | 16.83 (6.0%) | 16.33 (2.5%) | 18.39 (5.8%) |
| 64 KiB serial | 31.27 (13.2%) | 33.17 (6.3%) | 36.11 (10.0%) | 33.03 (4.4%) |
| 1 MiB bulk | 403.96 (19.6%) | 505.19 (31.5%) | 810.31 (31.0%) | 682.28 (37.3%) |
| 8 MiB bulk | 4833.54 (9.5%) | 3983.73 (23.7%) | 9129.05 (15.3%) | 9493.57 (14.2%) |
| 8 concurrent, 1 KiB each | 81.47 (7.3%) | 93.83 (4.6%) | 85.39 (2.4%) | 91.04 (9.2%) |
| 32 concurrent, 1 KiB each | 445.72 (24.0%) | 630.01 (5.8%) | 631.49 (6.0%) | 631.82 (4.2%) |

### 32-Connection Multicore TCP

One KiB exchange per connection, 32 connections, external controller submits and
joins one job per connection. **us/batch**.

| Workers | Weave Affine | Weave Stealing | Asio Shared | Asio Sharded | libuv Affine | uSockets/libuv Affine |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 339.46 (5.5%) | 319.16 (3.2%) | 346.69 (13.3%) | 357.83 (4.0%) | 332.78 (3.2%) | 322.89 (8.8%) |
| 2 | 222.59 (6.9%) | 204.14 (23.8%) | 229.42 (6.4%) | 265.54 (24.4%) | 224.49 (20.1%) | 214.18 (7.2%) |
| 4 | 205.09 (2.7%) | 213.97 (7.1%) | 214.55 (2.8%) | 221.55 (4.2%) | 210.36 (5.7%) | 209.07 (6.3%) |
| 8 | 175.80 (4.4%) | 188.84 (2.3%) | 181.18 (3.0%) | 177.24 (2.0%) | 169.33 (1.6%) | 174.46 (4.7%) |

### 1,024-Connection TCP

32 roundtrips per connection, four asynchronous peer workers, **ms/batch**.
The half-second sweep calibrated only one timed batch per repetition for the
one-worker cases; the raw iteration counts are retained. The longer followup
above covers the four-worker configurations.

| Workers | Weave Affine | Weave Stealing | Asio Shared | Asio Sharded | libuv Affine | uSockets/libuv Affine |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 635.64 (2.2%) | 629.27 (8.1%) | 623.35 (8.1%) | 639.67 (10.3%) | 628.35 (5.0%) | 642.07 (5.6%) |
| 4 | 184.14 (5.0%) | 180.52 (5.3%) | 198.89 (25.2%) | 185.89 (11.4%) | 184.94 (1.3%) | 187.06 (1.3%) |
| 8 | 188.81 (3.6%) | 180.85 (1.3%) | 195.32 (1.0%) | 190.08 (1.6%) | 185.97 (0.9%) | 190.64 (0.8%) |

### Balanced CPU

32 tasks, identical noinline integer work, **us/batch**. Native results include
the benchmark submission adapter, not a built-in libuv/uSockets task runtime.

| Workers | Weave Affine | Weave Stealing | Asio Shared | Asio Sharded | libuv Affine | uSockets/libuv Affine |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 3582.02 (3.7%) | 3625.43 (5.8%) | 3691.26 (2.5%) | 3644.54 (1.2%) | 3594.13 (4.5%) | 3579.91 (0.7%) |
| 2 | 1828.17 (0.8%) | 1809.72 (0.8%) | 1838.06 (0.7%) | 1847.43 (1.0%) | 1844.30 (2.6%) | 1853.05 (1.0%) |
| 4 | 942.05 (3.1%) | 928.66 (2.4%) | 947.32 (63.7%) | 964.38 (2.5%) | 942.61 (0.8%) | 951.30 (1.0%) |
| 8 | 517.94 (2.7%) | 505.00 (1.6%) | 516.03 (2.6%) | 522.22 (1.0%) | 507.84 (0.8%) | 507.97 (3.7%) |

### Skewed CPU

32 tasks, every Nth task performs 16 work calls, others one. **us/batch**.
Compare only within a worker-count row: total work changes with N. Fixed-affine
placement intentionally concentrates expensive tasks on one worker. Shared and
stealing schedulers can balance them, so cross-topology differences are not
general library performance rankings.

| Workers | Weave Affine | Weave Stealing | Asio Shared | Asio Sharded | libuv Affine | uSockets/libuv Affine |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 57732.01 (2.1%) | 57703.13 (0.8%) | 57687.52 (0.2%) | 57873.75 (28.3%) | 57524.87 (0.4%) | 57913.32 (0.5%) |
| 2 | 28865.59 (0.5%) | 16118.25 (0.8%) | 15605.93 (0.8%) | 28961.67 (1.7%) | 28915.29 (0.8%) | 28970.18 (0.4%) |
| 4 | 14427.38 (0.4%) | 4548.24 (0.4%) | 4744.62 (0.4%) | 14471.31 (0.5%) | 14431.32 (1.0%) | 14434.69 (0.4%) |
| 8 | 7218.25 (0.1%) | 2372.67 (1.3%) | 2299.99 (0.9%) | 7274.72 (0.8%) | 7224.70 (1.2%) | 7227.95 (0.3%) |

### Coroutine-Only Call

**ns/call**, nested immediate noinline coroutine. Native callback dispatch would
not be an equivalent workload, so those entries are deliberately N/A.

| Weave | Asio | libuv | uSockets |
| --- | --- | --- | --- |
| 11.28 (0.8%) | 16.69 (0.4%) | N/A | N/A |

## All Weave Controls

Initial full-sweep before/after results, including noisy cases. All use five
repetitions at a 0.5 s minimum; refer to the longer followup for its selected
controls. Matched Asio means the post-change baseline with the corresponding
workload and affinity/shared topology.

| Case (Unit) | Before (CV) | After (CV) | Change | Matched Asio (CV) |
| --- | --- | --- | --- | --- |
| `Weave/1024` (us) | 15.56 (8.1%) | 18.22 (8.0%) | 17.10% | 16.83 (6.0%) |
| `Weave/64` (us) | 17.17 (5.3%) | 17.12 (3.0%) | -0.31% | 16.95 (45.1%) |
| `Weave/65536` (us) | 33.29 (7.9%) | 31.27 (13.2%) | -6.07% | 33.17 (6.3%) |
| `WeaveAffineSkew/1` (us) | 57666.40 (0.7%) | 57732.01 (2.1%) | 0.11% | 57873.75 (28.3%) |
| `WeaveAffineSkew/2` (us) | 28948.20 (1.3%) | 28865.59 (0.5%) | -0.29% | 28961.67 (1.7%) |
| `WeaveAffineSkew/4` (us) | 14424.11 (0.6%) | 14427.38 (0.4%) | 0.02% | 14471.31 (0.5%) |
| `WeaveAffineSkew/8` (us) | 7222.97 (1.6%) | 7218.25 (0.1%) | -0.07% | 7274.72 (0.8%) |
| `WeaveBulk/1048576` (us) | 342.12 (7.9%) | 403.96 (19.6%) | 18.08% | 505.19 (31.5%) |
| `WeaveBulk/8388608` (us) | 4667.82 (9.4%) | 4833.54 (9.5%) | 3.55% | 3983.73 (23.7%) |
| `WeaveConcurrent<32>/1024` (us) | 552.17 (27.6%) | 445.72 (24.0%) | -19.28% | 630.01 (5.8%) |
| `WeaveConcurrent<8>/1024` (us) | 81.07 (4.1%) | 81.47 (7.3%) | 0.49% | 93.83 (4.6%) |
| `WeaveCoroutine` (ns) | 12.27 (27.3%) | 11.28 (0.8%) | -8.05% | 16.69 (0.4%) |
| `WeaveManyConnections/connections:1024/workers:1` (ms) | 619.44 (21.0%) | 635.64 (2.2%) | 2.62% | 639.67 (10.3%) |
| `WeaveManyConnections/connections:1024/workers:4` (ms) | 180.64 (1.9%) | 184.14 (5.0%) | 1.94% | 185.89 (11.4%) |
| `WeaveManyConnections/connections:1024/workers:8` (ms) | 189.66 (1.3%) | 188.81 (3.6%) | -0.45% | 190.08 (1.6%) |
| `WeaveMulticoreCpu/1` (us) | 3555.64 (1.4%) | 3582.02 (3.7%) | 0.74% | 3644.54 (1.2%) |
| `WeaveMulticoreCpu/2` (us) | 1846.08 (3.5%) | 1828.17 (0.8%) | -0.97% | 1847.43 (1.0%) |
| `WeaveMulticoreCpu/4` (us) | 935.28 (0.8%) | 942.05 (3.1%) | 0.72% | 964.38 (2.5%) |
| `WeaveMulticoreCpu/8` (us) | 501.49 (2.0%) | 517.94 (2.7%) | 3.28% | 522.22 (1.0%) |
| `WeaveMulticoreTcp/1` (us) | 311.39 (3.3%) | 339.46 (5.5%) | 9.01% | 357.83 (4.0%) |
| `WeaveMulticoreTcp/2` (us) | 207.61 (3.0%) | 222.59 (6.9%) | 7.22% | 265.54 (24.4%) |
| `WeaveMulticoreTcp/4` (us) | 202.28 (2.5%) | 205.09 (2.7%) | 1.39% | 221.55 (4.2%) |
| `WeaveMulticoreTcp/8` (us) | 171.19 (20.9%) | 175.80 (4.4%) | 2.69% | 177.24 (2.0%) |
| `WeaveStealingCpu/1` (us) | 3642.30 (1.4%) | 3625.43 (5.8%) | -0.46% | 3691.26 (2.5%) |
| `WeaveStealingCpu/2` (us) | 1812.57 (35.8%) | 1809.72 (0.8%) | -0.16% | 1838.06 (0.7%) |
| `WeaveStealingCpu/4` (us) | 931.59 (28.4%) | 928.66 (2.4%) | -0.31% | 947.32 (63.7%) |
| `WeaveStealingCpu/8` (us) | 501.02 (25.3%) | 505.00 (1.6%) | 0.79% | 516.03 (2.6%) |
| `WeaveStealingManyConnections/connections:1024/workers:1` (ms) | 610.40 (21.0%) | 629.27 (8.1%) | 3.09% | 623.35 (8.1%) |
| `WeaveStealingManyConnections/connections:1024/workers:4` (ms) | 178.77 (6.4%) | 180.52 (5.3%) | 0.98% | 198.89 (25.2%) |
| `WeaveStealingManyConnections/connections:1024/workers:8` (ms) | 184.93 (5.2%) | 180.85 (1.3%) | -2.20% | 195.32 (1.0%) |
| `WeaveStealingSkew/1` (us) | 57476.90 (0.3%) | 57703.13 (0.8%) | 0.39% | 57687.52 (0.2%) |
| `WeaveStealingSkew/2` (us) | 16140.29 (1.0%) | 16118.25 (0.8%) | -0.14% | 15605.93 (0.8%) |
| `WeaveStealingSkew/4` (us) | 4580.07 (0.6%) | 4548.24 (0.4%) | -0.70% | 4744.62 (0.4%) |
| `WeaveStealingSkew/8` (us) | 2355.56 (1.2%) | 2372.67 (1.3%) | 0.73% | 2299.99 (0.9%) |
| `WeaveStealingTcp/1` (us) | 336.00 (46.2%) | 319.16 (3.2%) | -5.01% | 346.69 (13.3%) |
| `WeaveStealingTcp/2` (us) | 197.44 (1.6%) | 204.14 (23.8%) | 3.39% | 229.42 (6.4%) |
| `WeaveStealingTcp/4` (us) | 207.76 (3.2%) | 213.97 (7.1%) | 2.99% | 214.55 (2.8%) |
| `WeaveStealingTcp/8` (us) | 183.67 (0.5%) | 188.84 (2.3%) | 2.82% | 181.18 (3.0%) |

## Evidence And Validation

- [before.json](before.json): 68 cases, 340 raw samples, zero errors.
- [after.json](after.json): 120 cases, 600 raw samples, zero errors.
- [confirmation-after.json](confirmation-after.json): 20 cases, 100 raw samples, zero errors.
- [confirmation-before.json](confirmation-before.json): 12 cases, 60 raw samples, zero errors.
- [environment.json](environment.json): versions, executable/data SHA-256 hashes,
  run order, build flags, machine details, filters, and dirty worktree context.
- Debug and Release CTest: 3/3 targets passed in each configuration.
- ASan CTest: 3/3 targets passed, including all 120 smoke cases. Benchmark,
  libuv, uSockets, and the adapters were instrumented.
- Fixture tests additionally passed ten consecutive ASan runs. There are 33
  runtime/core test cases and 15 benchmark fixture cases (ten new templated
  native-client cases across the two libraries).

Fixtures test fragmented/backpressured persistent transfers, refused/pending
connects, EOF/corrupt responses, cancellation with live write buffers, reusable
cross-thread submissions, and shutdown with active callbacks. The pinned
uSockets Windows backend needed an adapter-side SO_ERROR/getpeername check in
on_open to avoid treating refused connections as successful setup. This is
untimed; upstream library sources are unmodified.

No builds, tests, or heavy diagnostic queries were run concurrently with the
timed processes. The desktop host was not isolated, and residual scheduling,
TCP, peer-thread, and thermal effects remain. Raw individual samples were
validated; aggregate-only reporting was not used. Earlier dirty runtime changes
were preserved. The saved executable makes this a working-tree comparison, not
a clean-commit A/B.

Reproduce the full current suite:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/bench.ps1 -Seconds 0.5 -Repetitions 5
# The same six topologies at 1,024 connections:
powershell -ExecutionPolicy Bypass -File scripts/bench.ps1 -Filter ManyConnections -Seconds 1 -Repetitions 5
```

The exact focused followup regex is in environment.json. The pre-change
executable remains locally at `out/native-baselines/before.exe`; build products
are not committed as benchmark evidence.
