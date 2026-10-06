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
