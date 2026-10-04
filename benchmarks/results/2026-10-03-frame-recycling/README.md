# Coroutine frame recycling

Windows 11 build 22631, Ryzen 9 9900X, MSVC 19.44.35221 x64 Release.
Standalone Asio 1.36.0 and Google Benchmark 1.9.4, pinned as before.
See environment.json for machine, executable hashes, Git base, and dirty status.
These are local exploratory measurements, not production performance gates.

## Implementation

Async now uses a thread-local, size-class recycling allocator. It retains at
most 64 dead frames in each of four classes: 128, 256, 512, and 1024 bytes.
Retained payload is bounded by 120 KiB per thread. Oversized/over-aligned frames
and cache misses use the heap. Cache contents are freed at thread exit.

There are no references to the allocating thread in live frames. Unstarted
coroutines can outlive their creation thread and be destroyed elsewhere.
Allocation during TLS teardown uses a rounded heap fallback so later destruction
on another thread is still safe. Socket/Context thread affinity is unchanged.
Active or suspended frames never enter the free lists. OOM still terminates.

ASan builds poison dead cached storage and padding, and unpoison on reuse.
The implementation follows the [manual poisoning interface](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-runtime#custom-allocators-and-the-addresssanitizer-runtime).
WEAVE_RECYCLE_FRAMES=OFF keeps a normal-heap diagnostic path. Public Async/Result
usage and the previous IOCP scheduling/transfer policies did not change.

A 32-entry-per-class candidate still made about 32 heap allocations per
32-connection batch. The 64-entry cap removed those warmed misses. The earlier
profile is preserved as profile-32-entry-cache.json, not used for final timing.

## Full-suite timing

Arithmetic means across ten repetitions per case: two runs of five, each with
0.3s minimum per repetition and randomized case interleaving. Executables ran
sequentially: before, after, repeat-after, repeat-before. No builds or other
benchmarks overlapped these four runs. The original pre-change executable was
retained before editing. An initial run overlapping a build was discarded and
is not included here. Both source versions were dirty worktrees, not independent
clean release revisions.

Lower is better. Change is (after / before - 1). Asio is the normal, unmodified
Asio in the two after runs, averaged the same way. No disabled-recycling Asio
result is used in this table.

| Workload | Weave before | Weave after | Change | Asio |
| --- | ---: | ---: | ---: | ---: |
| Coroutine call | 27.11 ns | 10.81 ns | -60.1% | 16.03 ns |
| Echo 64 B | 15.230 us | 14.954 us | -1.8% | 15.061 us |
| Echo 1 KiB | 15.329 us | 15.178 us | -1.0% | 15.133 us |
| Echo 64 KiB | 29.835 us | 29.240 us | -2.0% | 29.620 us |
| Bulk 1 MiB | 323.149 us | 327.662 us | +1.4% | 428.189 us |
| Bulk 8 MiB | 3.154 ms | 3.041 ms | -3.6% | 3.288 ms |
| 8-connection batch | 78.573 us | 76.305 us | -2.9% | 84.271 us |
| 32-connection batch | 296.617 us | 287.673 us | -3.0% | 327.970 us |

Pooled repetition coefficients of variation (sample standard deviation / mean):

| Workload | Weave before | Weave after | Asio after |
| --- | ---: | ---: | ---: |
| Coroutine | 1.36% | 2.64% | 1.58% |
| Echo 64 B | 3.65% | 2.11% | 3.97% |
| Echo 1 KiB | 1.52% | 1.41% | 1.24% |
| Echo 64 KiB | 5.44% | 1.69% | 1.01% |
| Bulk 1 MiB | 8.99% | 7.94% | 23.03% |
| Bulk 8 MiB | 6.20% | 6.88% | 10.88% |
| Batch 8 | 2.43% | 1.72% | 2.67% |
| Batch 32 | 4.90% | 2.34% | 1.92% |

The coroutine improvement is large and repeatable. Small network changes,
including the apparent 1 MiB regression, do not establish a causal effect.
The first run alone suggested a 7% bulk improvement; the reverse-order repeat
did not support that claim. Do not extrapolate the coroutine win into a general
network-throughput claim. These are warm, reused-connection workloads, not
cold-allocation or connection-churn tests.

## Allocation controls

Seven 0.3s repetitions of the coroutine-only case in separate control runs:

| Configuration | Mean time |
| --- | ---: |
| Original Weave | 28.9 ns |
| Normal Asio | 16.4 ns |
| Asio with ASIO_DISABLE_AWAITABLE_FRAME_RECYCLING | 32.6 ns |
| Weave with the initial 32-entry cache | 11.1 ns |
| New Weave source with WEAVE_RECYCLE_FRAMES=OFF | 30.3 ns |

The Asio-disable experiment supports frame reuse as a major factor in the
original microbenchmark gap. It is a diagnostic, not a deliberately weakened
performance baseline. The Weave-disable experiment also reverses the benefit.
Raw files: recycling-control.json, recycling-disabled.json, pooled-micro.json,
and weave-recycling-disabled.json. The final 64-entry cache is measured in the
full-suite table above. Some older diagnostic files predate configuration fields
being embedded in the JSON; the configurations are recorded here explicitly.

## Instrumented allocation counts

profile-no-recycling.json and profile-final.json use the same new source with
recycling off/on, WEAVE_PROFILE_RUNTIME=ON, three 0.15s repetitions. Their
timings are not valid comparisons against uninstrumented Asio.

| Workload | Logical frame requests per iteration | Heap allocations, off | Heap allocations, on |
| --- | ---: | ---: | ---: |
| Coroutine | ~1 | ~1 | 0 |
| Bulk 1 MiB | ~5 | ~5 | 0 |
| Bulk 8 MiB | ~5 | ~5 | 0 |
| Batch 8 | ~41 | ~41 | 0 |
| Batch 32 | ~161 | ~161 | 0 |

Google Benchmark calibration warms the thread-local cache. Zero here refers
only to Weave coroutine heap allocations during these warmed measured wrappers,
not process-wide allocation, startup, or a universal zero-allocation guarantee.
Root-frame requests are amortized into the fractional counts in the JSON.

## Remaining bulk cost

The final instrumented 8 MiB mean was 3.394 ms, split approximately as follows:

| Timed wall-clock region | Mean | Share |
| --- | ---: | ---: |
| WSASend submission | 2.097 ms | 62% |
| WSARecv submission | 0.740 ms | 22% |
| GetQueuedCompletionStatusEx, including blocking | 0.545 ms | 16% |

This identifies where elapsed time occurs, not CPU instructions, kernel CPU
ownership, or why Asio differs. Instrumentation adds clock-read overhead.
WPR CPU sampling failed with 0xc5585011 (system profiling policy); no CPU-sample
trace was collected, and WPR was confirmed not recording afterward. Native
submission cost, copying, and scheduling need further investigation before
another networking optimization is justified.

## Correctness

Debug and Release CTest passed, including all benchmark smoke cases. MSVC ASan
CTest passed with recycling enabled; the no-recycling configuration passed too.
There are 16 normal test cases and one additional ASan poisoning case. New tests
cover size boundaries, live-block non-aliasing, capacity limits, cache draining,
oversized frames, over-aligned promises, cross-thread destruction, source-thread
exit, teardown allocation fallback, exact-once parameter destruction, and manual
poison/unpoison state. Existing pending I/O, cancellation, EOF, partial transfer,
queued/inline success, and large-duplex-transfer tests remain enabled.
