# Windows CI benchmarks

[Workflow](../.github/workflows/benchmarks-windows.yml) compares Weave, standalone
Asio 1.36.0 and Tokio 1.53.2 on code pushes to `main`, pull requests, and manual
dispatch. README/doc-only changes skip measurements. The separate correctness
workflow still runs Debug, Release and ASan without native benchmark execution.
Linux measurements will follow its backend implementation.

## Fixed, short matrix

Protocol: `windows-tcp-ci-v1`. Each case runs **seven one-second windows** for
each library: **84 seconds of timed work** total. Startup, per-connection warmup,
sorting, reporting, and cleanup add overhead. The measurement supervisor owns
every descendant in a Windows Job and enforces a **300-second hard wall-clock
limit**, including startup and cleanup. Configure/build and publication are
separate; a cold dependency/toolchain build is not promised to fit five minutes.
The benchmark job has a 12-minute outer limit. Dependency caches reduce warm builds.

| Case | Connections | Request/response | CPU work |
| --- | ---: | ---: | --- |
| Small-message latency | 64 | 1 KiB each way | None |
| Concurrent connections | 1,024 | 1 KiB each way | None |
| Bulk transfer | 64 | 64 KiB each way | None |
| Uneven scheduling | 256 | 1 KiB each way | 20,000 identical integer steps on every eighth connection |

The [public Windows runner currently provides four vCPUs](https://docs.github.com/en/actions/reference/runners/github-hosted-runners).
We select two server and two client cores when four distinct reported physical
cores are available, or one each when only two/three are available. Each selected
core contributes one logical processor; server/client never share reported SMT
siblings. All libraries use the same worker budget and affinity masks. Fewer than
two usable physical cores or multiple processor groups is a setup failure, not
a silent oversubscribed fallback. VM topology is what Windows reports, not a
guarantee about physical host isolation. README and evidence identify the actual
budget; these results are not the manual four-worker/12-core profile.

## Like For Like

- Release x64, exceptions disabled for both C++ servers, no profiling/ASan. Rust
  1.94.0, Cargo `--locked`, optimized Release with panic abort. Asio and Tokio are
  pinned; only the native TCP servers and common load generator are built. The CI
  CMake preset does not fetch Google Benchmark, libuv, uSockets, Trantor, or doctest.
- Weave uses its default sharded IOCP with work stealing; Asio a shared
  `io_context`; Tokio its multi-thread runtime. Each uses its normal coroutine
  API, a persistent task/buffer per connection, exact-frame reads, and complete
  writes. No per-request task spawning or library-specific extra CPU work.
- One separate native Asio load-generator executable drives every backend with
  the same payload validation, workers, socket settings and latency collection.
  Every returned frame is checked. Every connection must progress during measurement.
- Loopback IPv4, persistent connections, TCP_NODELAY, native backlog hint 8,192.
  All connections complete eight warmup exchanges plus 250 ms of validated,
  untimed traffic per connection before GO. Establishment,
  warmup, latency sorting and teardown are outside the timed window. A new server
  and client process are used for each sample; no language has a JIT warmup advantage.
- Fixed-seed randomized workload/backend order within complete matched repetition
  blocks reduces ordering bias. All 84 samples are required; duplicates, missing
  cases, invalid counters, stalled connections or windows exceeding two seconds
  fail the run. Partial evidence is retained, never published as complete results.

## Variance And Interpretation

README shows median round trips/second and the largest throughput coefficient of
variation (sample standard deviation / mean) among the three libraries for each
case. Workflow summaries/artifacts also contain p50/p99/p99.9 RTT, CPU use, CPU
cycles per completed round trip, private memory, and per-metric between-window CV.
Latency summaries are medians of per-window percentiles, not pooled percentiles.

Weave/Asio and Weave/Tokio throughput ratios pair samples by repetition. A
fixed-seed, 20,000-resample whole-block bootstrap supplies a **90% interval** for
the median ratio. Seven repetitions are a bounded diagnostic sample, not proof
of tiny regressions or a simultaneous confidence guarantee for the full matrix.
An interval including 1 does not establish a throughput winner.

Predeclared precision warnings: throughput CV >10%, p99 CV >25%, or a paired
interval width exceeding 20% of its median ratio. README identifies throughput
noise separately from p99 noise: noisy comparisons are inconclusive for the
affected metric and library/workload, not every measurement in the run. A p99
warning does not invalidate otherwise stable throughput. We retain and publish every sample,
including outliers, with **no adaptive stopping, automatic reruns, or cherry-picked
best runs**. Noise is not a correctness failure and does not fail a PR. Protocol,
payload, process, or deadline failures do fail it. This workflow is a comparison
dashboard, not a before/after non-regression gate; a new VM is not a controlled
historical baseline.

Hosted-runner contention, clock changes, and the common client's capacity can
limit precision. Client CPU occupancy >=90% of its core budget is flagged; absence
of that flag does not prove an unlimited client. Windows GetProcessTimes can be
quantized, including zero deltas; CPU seconds remain diagnostic, and measured
cycles are never converted to seconds. These closed-loop tests have one outstanding
request per connection and no coordinated-omission correction. They do not measure
open-loop arrival latency, internet performance, connection setup, cancellation,
or TLS/HTTP. No short CI benchmark can eliminate all host variance.

## Latest Results

Each complete main run updates only the marked benchmark block in README, with
its source revision, timestamp, workload medians, variance warning, and workflow
link. No hand-entered or synthetic results are advertised as measured CI evidence.
PR runs publish job summaries and artifacts only, with read-only permissions.
Publication is a separate trusted-main-only job with `contents: write`; it validates
the complete sample matrix again, rejects dirty/stale source revisions, and uses a
normal fast-forward push. It never executes PR code with a write token. Branch
protection must permit the bot to update README; otherwise that step fails visibly
and the complete results remain available in Actions.

The [repository GITHUB_TOKEN does not recursively trigger workflows on its push](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow).
Benchmark outputs and logs remain ignored under `benchmarks/results/` and are
uploaded as `windows-benchmarks` artifacts for 30 days. The compact README table
persists; download raw evidence before artifact retention expires.

## Run Locally

Windows, Visual Studio 2022, CMake 3.25+, Python 3.11+, Git, and Rustup:

```sh
cmake --preset windows-bench-ci
cmake --build --preset bench-ci --parallel 4
rustup toolchain install 1.94.0 --profile minimal
rustup run 1.94.0 cargo build --release --locked --manifest-path modules/tcp/benchmarks/tokio/Cargo.toml --target-dir build/tokio-ci
python scripts/bench_ci.py run
```

Local runs do not modify README or push anything. `summary.md`, `analysis.json`,
individual process logs, all samples, source/binary hashes, compiler versions,
topology, masks and environment metadata are saved in a new ignored output folder.
The [manual stress comparison](../modules/tcp/benchmarks/tokio/README.md) and
[migration gate](gate.md) remain separate opt-ins.
