# Tokio Runtime Comparison

Optional manual Windows TCP comparison: pinned Tokio 1.53.2, Weave and Asio.
Rust is needed only for this benchmark, not Weave or correctness CI.
Cargo.lock pins the dependency graph.

```sh
cmake --preset windows-runtime-bench
cmake --build --preset runtime-bench --parallel 4
rustup run 1.94.0 cargo build --release --locked --manifest-path modules/tcp/benchmarks/tokio/Cargo.toml --target-dir build/tokio
python scripts/bench_scaling.py run
```

[Scaling protocol, hardware limits, publication and latest results](../../../../docs/runtime-scaling.md)
is the canonical guide. It compares sequential fresh-process windows and retains
every sample, CPU/cycles, RTT tails, checksums and source/binary provenance.

The separate fixed-four-worker stress driver uses eight physically separate client
cores and requires twelve accessible physical cores in one processor group:

```sh
python scripts/bench_tokio.py
python scripts/bench_tokio.py --smoke --duration-ms 100 --repetitions 1 --timeout-seconds 60
cargo test --locked --manifest-path modules/tcp/benchmarks/tokio/Cargo.toml --target-dir build/tokio
```

Persistent connections use fixed complete requests/responses, reusable buffers,
TCP_NODELAY and validated payloads. Workloads are 1,024/4,096 x 1 KiB, 256 x 64 KiB,
and uneven CPU work. Tokio's accept loop runs on a worker. Windows backlog uses
SOMAXCONN_HINT(8192), not a guarantee of queue capacity.

The fixed driver defaults to seven randomized two-second blocks with a five-minute
owned-process-tree deadline. Setup/warmup/cleanup are untimed and failures retain
phase/process diagnostics. Smoke is a protocol check, not timing evidence.
Closed-loop localhost tails lack coordinated-omission correction and do not prove
open-loop SLOs or maximum capacity. CPU time can underreport; cycles are not seconds.

Raw output stays ignored under benchmarks/results. Do not run builds/tests during
measurement, overwrite evidence, discard outliers or treat non-reproduction as a fix.
[Earlier stress campaigns and retained failures](https://github.com/c-schembri/weave/blob/4f24d614b31708b10007072b2b9e6aa323249bad/modules/tcp/benchmarks/tokio/README.md)
remain in Git history rather than duplicated result tables here.
