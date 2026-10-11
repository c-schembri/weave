# Local Runtime Scaling

Compare Weave, standalone Asio 1.36.0, and Tokio 1.53.2 on Windows using their
normal runtime APIs. Weave uses work stealing and its default sharded IOCP.
Asio runs one shared io_context; Tokio uses its multi-thread runtime. There is
one worker per selected server physical core, not oversubscribed worker counts.
The requested counts are **1, 2, 4, 8, 16, 32**.

## Hardware

Keep a fixed set of separate physical cores for the identical Asio load generator.
The default reserves four client cores for the entire sweep. Server masks grow
within the remaining cores; client placement and worker count never change.
Use one logical processor per physical core and never share SMT siblings between
processes. Physical isolation here means affinity placement, not a reservation
against unrelated applications; avoid builds, tests and other load while measuring.

The local Ryzen 9 9900X has 12 physical cores / 24 logical processors, so it can
measure 1/2/4/8 server cores with four client cores. The report lists 16/32 as
unavailable, not failed measurements or simulated core counts. A bigger machine
can measure them if server + client physical cores fit. Currently the harness
requires one Windows processor group; multi-group affinity is not implemented.

## Protocol

`windows-runtime-scaling-v1`: seven independent two-second windows per
library/core-count/workload by default, plus eight validated warmup exchanges and
250 ms of untimed traffic per connection. All libraries run sequentially; each
server/client pair is cleaned up before the next starts. Every repetition includes
every supported core count, workload and backend once, with fixed-seed randomized
ordering to reduce ordering and thermal bias. No adaptive stopping or selective
retries. All samples and outliers are retained.

| Workload | Connections | Frame each way | Server CPU work |
| --- | ---: | ---: | --- |
| Small frames | 1,024 | 1 KiB | None |
| Many connections | 4,096 | 1 KiB | None |
| Large frames | 256 | 64 KiB | None |
| Uneven scheduling | 1,024 | 1 KiB | 20,000 identical integer steps on every eighth connection |

Persistent IPv4 loopback connections, TCP_NODELAY, and the native backlog hint
8,192. Each server reads an exact request and writes a complete response, reusing
a per-connection buffer. One request is outstanding per connection. The common
client validates every returned byte and requires measured progress from every
connection. Setup, warmup, sorting and cleanup are outside the measurement window.

The 1/2/4/8-core sweep contains 336 windows and 11.2 minutes of timed work; setup
and cleanup add overhead. The full six-count matrix has 504 windows. The default
35-minute supervisor deadline owns every descendant in a Windows Job and kills
the tree on failure/timeout. The separate migration gate still has its five-minute
deadline. Neither benchmark collector runs in correctness CI.

## Interpretation

README shows the 1,024-client small-frame sweep; the uploaded summary includes
all workloads, p99/p99.9 RTT, client CPU occupancy, server cycles/op, private memory,
and paired throughput ratios against Asio and Tokio. Scaling speedup compares a
backend to its own one-core result in matched repetition blocks; efficiency is
speedup divided by server cores. Medians of per-window percentiles are not pooled
latency. Exploratory 90% whole-block bootstrap intervals use 20,000 fixed-seed
resamples and are not simultaneous guarantees for the entire matrix.

Flag throughput CV >10%, p99 CV >25%, or scaling-interval width >20% of its median.
Noisy comparisons are inconclusive for the affected metric; p99 noise does not
invalidate stable throughput. Client occupancy >=90% of its fixed core budget is
flagged. An unflagged row does not prove unlimited client capacity. Client-busy
rows describe end-to-end loopback throughput, not maximum server capacity or proof
of poor runtime scaling. Longer runs do not remove load-generator or kernel ceilings.

Windows CPU-time counters can be quantized or zero even when measured process
cycles are substantial. Keep CPU time diagnostic; never convert cycles to seconds
or interpret zero CPU time as zero CPU cost. These are closed-loop measurements,
not open-loop service latency, internet throughput, or universal library rankings.

## Run And Publish

Windows, Visual Studio 2022, CMake 3.25+, Python 3.11+, Git, Rust/Cargo:

```sh
cmake --preset windows-runtime-bench
cmake --build --preset runtime-bench --parallel 4
rustup run 1.94.0 cargo build --release --locked --manifest-path modules/tcp/benchmarks/tokio/Cargo.toml --target-dir build/tokio
python scripts/bench_scaling.py run
```

Commit source changes before collecting publishable evidence. The narrow CMake
preset fetches only pinned Asio; no doctest, Google Benchmark, libuv, uSockets or
Trantor. Rust/Cargo is a benchmark-only dependency, never a Weave dependency.
Each run creates a new ignored `benchmarks/results/scaling-*` directory containing
environment/source/binary hashes, affinity masks, every sample, per-process logs,
full analysis and summary. Existing evidence is never overwritten. Partial or
smoke runs cannot be published as completed performance measurements.

Each actual measurement window must be between the requested duration and one
second beyond it. A longer outstanding exchange fails timing validation, even if
the process eventually completes and every response is correct. Never omit that
sample or selectively retry it. A complete failed sweep can be uploaded explicitly
with `package --diagnostic`; it is labelled failed, retains every raw sample, and
stores separately named diagnostic analysis with its own analyzer provenance.
Affected cohorts are non-comparable, not promoted into passed performance evidence.
Structural errors, incomplete matrices and smoke runs still cannot be packaged.

```sh
# Protocol/cleanup check only, not performance evidence.
python scripts/bench_scaling.py run --smoke --duration-ms 100 --repetitions 1 --warmup-ms 50 --timeout-seconds 120

python scripts/bench_scaling.py package --directory benchmarks/results/scaling-TIMESTAMP --output benchmarks/results/runtime-scaling.zip --evidence-url https://github.com/c-schembri/weave/releases/tag/benchmarks-TIMESTAMP
```

Packaging revalidates the complete matrix and reproduces the stored analysis from
raw samples. Upload the ZIP as a benchmark-only GitHub prerelease asset, not a
library release or tracked JSON/log directory. Link it from the curated README
table with the measured revision, timestamp, hardware and sampling windows.
Publishing never triggers fresh measurements or blocks correctness CI.

## Optimization Comparisons

Preserve the original server executable before rebuilding. Supply its original
clean-source `environment.json`; the collector verifies that the executable hash
matches that archived evidence. The before binary is rerun inside every matched
block, not compared with a previous day's numbers. `--shared-candidate` separately
measures the existing optional shared IOCP layout; the default remains sharded.
The archived before backend uses sharded IOCP: shared-candidate comparisons
measure the combined layout/runtime change, not an isolated fast-path improvement.

```sh
python scripts/bench_scaling.py run --before-binary build/runtime-before/Release/weave_runtime_server.exe --before-evidence benchmarks/results/scaling-TIMESTAMP/environment.json --shared-candidate --timeout-seconds 2700
```

This five-backend 1/2/4/8-core matrix has 560 two-second windows, or 18.7 minutes
of timed work plus setup/cleanup. Every case uses the same four physical client
cores. The raw report includes both candidates' matched comparisons against the
before binary, Asio and Tokio, and every observed outlier. Neither additional
backend changes the baseline library configuration or payload/validation work.

For stall diagnosis, use [execution tracing](#diagnostic-tracing). Traces and
profile builds are never accepted as timing evidence. On this 12-core machine,
client saturation still limits peak-capacity conclusions; a separate load-generator
machine is required before claiming maximum server scaling. No such remote
capacity or matched-offered-load measurement has been performed by this protocol.

## Latest Results

The [2026-10-06 campaign](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261006-182604)
passed all 560 timing windows. Default-layout throughput changes for 1,024 x 1 KiB
were -0.5%, -0.4%, -0.2% and +0.5% at 1/2/4/8 cores, mostly measurement noise.
The [README](../README.md#local-runtime-benchmarks) retains absolute medians.
This does not show Weave consistently beating Asio/Tokio.

Shared IOCP versus the **old sharded default** combines layout and runtime changes:

| Cores | Throughput | Server cycles/RTT | P99 | P99.9 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | +0.3% | -0.3% | +0.2% | -0.8% |
| 2 | +5.1% | -2.3% | -4.4% | -6.5% |
| 4 | +12.3% | -4.4% | -8.8% | -8.7% |
| 8 | -18.2% | -14.4% | +20.1% | +13.1% |

Values are medians of matched ratios, not aggregate-median ratios. Shared improves
four-core throughput here but regresses at eight; it stays opt-in. Client limits
affect four/eight-core conclusions. Evidence includes unfavorable results and
confidence intervals, not just this summary.

## Diagnostic Tracing

Build separately with `WEAVE_TRACE_RUNTIME=ON` and set `WEAVE_TRACE_DIRECTORY`
to an existing ignored directory. Per-thread PID/TID files hold a memory-mapped
262,144-entry QPC ring for enqueue/steal/execute/park/wake/submission/completion.
After all writers exit:

```sh
python scripts/trace_runtime.py benchmarks/results/trace-DIRECTORY --output benchmarks/results/trace-DIRECTORY/analysis.json
```

Wrapped history drops old boundaries; process termination can leave unmatched
begins. Idle IOCP waits are not evidence of stalls. Instrumentation changes costs
and is never performance evidence. An old 5.8-second timing failure remains in
[diagnostic evidence](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261006-172307);
non-reproduction does not identify or prove a fix. Older investigations remain
in Git history, not an expanding feature backlog.
