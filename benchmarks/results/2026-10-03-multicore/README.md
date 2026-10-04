# Multicore runtime baseline

Windows 11 build 22631, Ryzen 9 9900X (12 cores, 24 logical processors),
MSVC 19.44.35221 x64 Release. Asio 1.36.0 and Google Benchmark 1.9.4 use
the existing pinned revisions. Frame recycling is enabled for both libraries;
Weave profiling is disabled. See environment.json for executable hashes,
configuration, run order, and the dirty Git base.

## Implementation

Runtime adds one Context/IOCP per worker thread. spawn assigns tasks round-robin;
spawn_on selects a worker. Tasks and sockets remain on their worker. JoinHandle
supports external blocking get and asynchronous joins that resume on the caller's
Context. Shutdown cancels worker-owned socket I/O and drains accepted tasks.
The standalone single-threaded Context remains available without a worker pool.

This is not a work-stealing scheduler or Tokio feature parity. It does not
migrate tasks/sockets, automatically distribute accepted connections, provide
individual task abort, or isolate blocking work. These measurements establish
a first scaling baseline, not that the scheduler is finished or faster in general.

## Multicore CPU

Arithmetic means of seven repetitions, 0.5s minimum per repetition, randomized
case interleaving (cpu-confirm.json). Time is per batch of 32 spawned tasks,
each executing the same 100,000 dependent integer updates. Task submission and
result joins are timed; worker startup/teardown are not. Asio uses a shared
io_context with N threads and co_spawn/use_future; Weave uses Runtime/JoinHandle.
Both check the result. Lower is better; CV is sample standard deviation / mean.

| Workers | Weave | Asio | Weave CV | Asio CV |
| --- | ---: | ---: | ---: | ---: |
| 1 | 3.661 ms | 3.710 ms | 1.23% | 1.24% |
| 2 | 1.846 ms | 1.844 ms | 1.90% | 1.37% |
| 4 | 0.955 ms | 0.929 ms | 1.33% | 1.11% |
| 8 | 0.499 ms | 0.505 ms | 1.43% | 1.18% |

Weave's throughput improves 7.34x from one to eight workers for this balanced
finite workload. Scaling is similar to Asio; this does not establish a general
advantage, heterogeneous load balancing, or fairness for long CPU-bound tasks.
The earlier multicore-repeat.json had roughly 40% CV for Weave/1 and Asio/8 CPU
cases. That noise prompted this longer CPU-only run; the earlier data is retained.

## Multicore TCP

Arithmetic means of five repetitions, 0.3s minimum, randomized case interleaving
(multicore-repeat.json). Time is per batch of 32 persistent-connection echoes,
1 KiB per connection, including external spawn/join. The same blocking EchoPeer
is used for each connection on both sides. Worker, buffer, and connection setup
and teardown are outside timing. Asio shared uses one context with N threads;
Asio sharded uses N contexts with one thread each, matching Weave's mapping.

| Workers | Weave | Asio shared | Asio sharded |
| --- | ---: | ---: | ---: |
| 1 | 334.869 us | 343.911 us | 357.712 us |
| 2 | 225.704 us | 220.186 us | 240.187 us |
| 4 | 208.951 us | 212.921 us | 211.906 us |
| 8 | 176.991 us | 180.463 us | 182.549 us |

| Workers | Weave CV | Asio shared CV | Asio sharded CV |
| --- | ---: | ---: | ---: |
| 1 | 7.03% | 2.53% | 2.83% |
| 2 | 9.50% | 4.84% | 12.03% |
| 4 | 1.72% | 6.68% | 2.07% |
| 8 | 0.89% | 2.25% | 2.89% |

Weave improves 1.89x from one to eight workers in this harness, not 7x networking
throughput. Small differences from Asio are not a reliable overall win. The
32 peer threads, controller, and Windows TCP stack contribute to the timings.
This is not a remote-server, accept-distribution, saturation, or tail-latency test.
No previous implementation of these multicore cases exists for a before column;
the existing single-thread cases are compared separately below.

## Single-thread regression check

The original pre-change executable was saved before editing. Both versions
contain the prior frame-recycling work and are dirty worktrees, not separate
clean release commits. Executables ran sequentially, without overlapping builds
or correctness tests. The following means use legacy-repeat-after.json followed
by legacy-repeat-before.json: five repetitions each, 0.3s minimum, randomized
case interleaving. Change is (after / before - 1), so positive means slower.
Asio was not modified; both controls are shown to expose run-to-run variation.

| Workload | Weave before | Weave after | Change | Asio before | Asio after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Coroutine call | 10.913 ns | 10.996 ns | +0.8% | 16.457 ns | 16.721 ns |
| Echo 64 B | 18.447 us | 19.336 us | +4.8% | 17.426 us | 17.552 us |
| Echo 1 KiB | 16.640 us | 18.872 us | +13.4% | 16.751 us | 20.067 us |
| Echo 64 KiB | 30.456 us | 35.845 us | +17.7% | 31.935 us | 35.791 us |
| Bulk 1 MiB | 357.771 us | 363.889 us | +1.7% | 598.090 us | 832.382 us |
| Bulk 8 MiB | 4.241 ms | 4.430 ms | +4.5% | 3.959 ms | 4.160 ms |
| 8-connection batch | 82.741 us | 87.672 us | +6.0% | 90.570 us | 96.736 us |
| 32-connection batch | 415.471 us | 451.428 us | +8.7% | 644.622 us | 539.921 us |

| Workload | Weave before CV | Weave after CV | Asio after CV |
| --- | ---: | ---: | ---: |
| Coroutine call | 3.01% | 1.64% | 3.14% |
| Echo 64 B | 27.53% | 16.51% | 6.09% |
| Echo 1 KiB | 7.54% | 11.95% | 9.82% |
| Echo 64 KiB | 1.46% | 14.78% | 11.50% |
| Bulk 1 MiB | 8.39% | 8.89% | 29.92% |
| Bulk 8 MiB | 13.58% | 17.78% | 28.36% |
| 8-connection batch | 2.19% | 9.02% | 11.88% |
| 32-connection batch | 10.96% | 19.39% | 17.13% |

These are apparent regressions, not proof that all changes are noise. To check
the larger echo differences, a separate longer run measured only 1 KiB and
64 KiB echoes, before then after, seven repetitions at 0.5s (echo-confirm-*.json):

| Workload | Weave before | Weave after | Change | Asio before | Asio after |
| --- | ---: | ---: | ---: | ---: | ---: |
| Echo 1 KiB | 16.499 us | 16.030 us | -2.8% | 16.112 us | 15.837 us |
| Echo 64 KiB | 31.791 us | 29.844 us | -6.1% | 31.752 us | 31.025 us |

Weave CV was 4.14%/2.15% before/after for 1 KiB and 4.20%/1.77% for 64 KiB.
Asio CV was 5.93%/1.78% and 6.24%/2.92%. The earlier echo regression did not
reproduce. Neither this reversal nor the noisy full-suite comparison establishes
a causal speedup or a clean no-regression guarantee. Keep both, rather than
selecting only the favorable run. Isolated-host profiling is still needed for
small network changes; the CPU scaling result is substantially more stable.

## Raw runs and reproduction

All timed exploratory runs are retained, including noisy ones. Chronological order:

| File | Cases | Repetitions | Minimum seconds |
| --- | --- | ---: | ---: |
| before.json | Original 16 | 3 | 0.2 |
| after.json | All 36 | 3 | 0.2 |
| multicore-repeat.json | New 20 | 5 | 0.3 |
| after-legacy.json | Original 16, new executable | 3 | 0.2 |
| cpu-confirm.json | Multicore CPU 8 | 7 | 0.5 |
| legacy-repeat-after.json | Original 16, new executable | 5 | 0.3 |
| legacy-repeat-before.json | Original 16, saved executable | 5 | 0.3 |
| echo-confirm-before.json | Echo 1 KiB/64 KiB, saved executable | 7 | 0.5 |
| echo-confirm-after.json | Echo 1 KiB/64 KiB, new executable | 7 | 0.5 |

Every run uses --benchmark_enable_random_interleaving=true and JSON output.
Raw JSON records all iterations, aggregates, timestamps, and configuration flags.
No benchmark case reported an error. Files named before use the saved pre-change
binary; every other file uses the same post-change binary hash. The initial
three-repetition runs were too variable for fine-grained comparisons.

```powershell
cmake --build --preset release --parallel
.\build\windows\Release\weave_bench.exe --benchmark_filter=MulticoreCpu --benchmark_min_time=0.5s --benchmark_repetitions=7 --benchmark_enable_random_interleaving=true --benchmark_out=cpu.json --benchmark_out_format=json
.\build\windows\Release\weave_bench.exe --benchmark_filter=Multicore --benchmark_min_time=0.3s --benchmark_repetitions=5 --benchmark_enable_random_interleaving=true --benchmark_out=multicore.json --benchmark_out_format=json
.\build\windows\Release\weave_bench.exe '--benchmark_filter=^(Weave|Asio)(/|Coroutine|Bulk|Concurrent)' --benchmark_min_time=0.3s --benchmark_repetitions=5 --benchmark_enable_random_interleaving=true --benchmark_out=legacy.json --benchmark_out_format=json
```

Only real_time is used. Google Benchmark's controller-thread CPU counter does
not account for aggregate worker CPU. Repetition CV is not request tail latency.
See docs/benchmarks.md for the full methodology and remaining coverage gaps.

## Correctness

Debug and Release CTest pass: 27 doctest cases and all 36 benchmark smoke cases.
MSVC ASan CTest passes, including its additional allocator-poisoning case, and
passed ten consecutive repeat-until-fail runs. The multicore example returns 42.
The 11 new test cases cover actual parallel execution, stable worker affinity,
one-worker nested spawn/join, cross-worker and standalone-Context async joins,
move-only results, concurrent producers, detached handles, draining, stop/submit
races, worker-initiated stop, pending accept/read cancellation, explicit later-I/O
rejection, and worker-local destruction of captured sockets. TCP transfer tests
cover fragmented data on multiple workers with inline-success skipping on/off.
Existing EOF, partial transfer, cancellation, and allocator tests remain enabled.
ASan is not a data-race detector; this does not replace a ThreadSanitizer run.
