# PostgreSQL Benchmarks

## Latest Native Windows Run (2026-10-10)

Ryzen 9 9900X (12 physical / 24 logical cores), Windows 11, MSVC 19.44 Release,
native PostgreSQL 18.6, libpq 18.4 and OpenSSL 3.6.5. No WSL forwarding.
The owned database uses two physical cores including their SMT siblings;
the serial client uses one different physical core. This is a client comparison,
not a database capacity or durable-WAL benchmark. Unrelated services were not
stopped. **Before and percentage change are unavailable** for this implementation.

Seven paired sequential samples alternate implementation order, using common
iteration counts calibrated toward two seconds. Setup and cleanup are outside
query timing; connect includes connection/authentication/finish. All consumed
values and checksums matched. Protocol 3.0, required SCRAM, GSS disabled and
client certificates disabled are pinned for both clients. TLS additionally pins
verified TLS 1.3 and SCRAM-PLUS. Spread is `(max - min) / median`; throughput
comparisons above 10% spread in either client are **inconclusive**, not wins.
Windows process CPU time underreported sustained activity and is unavailable;
raw counters are retained, not converted into utilization claims.

### Plaintext

| Workload | Weave ops/s | libpq ops/s | W / PQ | Spread W / PQ | P99 us W / PQ |
| --- | ---: | ---: | ---: | ---: | ---: |
| Connect | 34 | 25 | 1.38x | 41.4 / 54.9% | 125868.6 / 128582.8 |
| Simple | 26,707 | 27,271 | 0.98x | 7.8 / 7.8% | 78.0 / 75.2 |
| Extended | 24,574 | 25,904 | 0.95x | 12.9 / 13.2% | 91.5 / 84.6 |
| Prepared | 27,829 | 27,950 | 1.00x | 4.9 / 7.3% | 69.3 / 67.3 |
| Binary | 24,681 | 25,417 | 0.97x | 8.8 / 8.7% | 88.0 / 87.1 |
| Batch | 144,209 | 194,740 | 0.74x | 6.4 / 8.2% | 10.9 / 9.1 |
| Rows | 1,003 | 1,407 | 0.71x | 3.3 / 5.1% | 1311.5 / 959.4 |
| COPY | 1,269 | 1,281 | 0.99x | 3.3 / 1.9% | 997.1 / 1019.8 |
| LO read | 5,292 | 5,838 | 0.91x | 21.8 / 9.7% | 311.9 / 281.8 |
| LO overwrite | 88 | 89 | 1.00x | 35.8 / 31.8% | 24424.2 / 27574.9 |
| LO append | 1,505 | 1,736 | 0.87x | 38.9 / 70.3% | 1790.8 / 2633.0 |

### Verified TLS

| Workload | Weave ops/s | libpq ops/s | W / PQ | Spread W / PQ | P99 us W / PQ |
| --- | ---: | ---: | ---: | ---: | ---: |
| Connect | 25 | 21 | 1.21x | 72.4 / 61.8% | 126924.1 / 127999.0 |
| Simple | 23,612 | 23,985 | 0.98x | 8.4 / 5.6% | 89.7 / 91.7 |
| Extended | 21,817 | 22,613 | 0.96x | 8.8 / 7.4% | 97.2 / 95.3 |
| Prepared | 24,708 | 24,078 | 1.03x | 6.5 / 12.7% | 76.7 / 83.4 |
| Binary | 20,184 | 20,520 | 0.98x | 10.6 / 7.0% | 144.3 / 121.8 |
| Batch | 118,279 | 159,924 | 0.74x | 12.6 / 10.0% | 15.0 / 12.6 |
| Rows | 438 | 476 | 0.92x | 4.2 / 7.3% | 2850.2 / 2659.2 |
| COPY | 1,127 | 1,135 | 0.99x | 6.8 / 10.6% | 1217.9 / 1202.3 |
| LO read | 1,753 | 1,857 | 0.94x | 6.2 / 4.9% | 817.3 / 774.2 |
| LO overwrite | 114 | 127 | 0.89x | 23.3 / 24.2% | 24494.7 / 18379.5 |
| LO append | 1,091 | 1,128 | 0.97x | 53.2 / 10.0% | 1407.8 / 1598.4 |

Batch throughput counts statements and its serial P99 is amortized per statement,
not whole-batch latency. Rows returns 1,000 rows with 256-byte text per query.
Large-object overwrite repeatedly updates the same object within a transaction:
its nonlinear database/MVCC work makes samples much longer than the pilot target.
It is not durable storage throughput. The complete serial/concurrent/memory run
took 40 minutes locally and is never an automatic CI requirement.

### Retained Result Memory

Matched fully consumed results remain owned during inspection. Values are bytes
reported by `ResultSet::memory_size()` / `PQresultMemorySize()`, not malloc call
counts, peak RSS, or identical exclusive allocation boundaries. Weave includes
its result/container/pool backing requests; the libpq reader does not retain a
second owning C++ row graph. Smaller/larger values cannot alone prove an allocator
or throughput advantage.

| Rows | Text bytes/row | Weave bytes | libpq bytes |
| ---: | ---: | ---: | ---: |
| 1 | 16 | 984 | 3,288 |
| 1 | 128 | 1,096 | 3,288 |
| 1 | 1,024 | 1,992 | 3,288 |
| 1,000 | 16 | 447,920 | 65,752 |
| 1,000 | 128 | 559,920 | 180,440 |
| 1,000 | 1,024 | 1,455,920 | 1,083,023 |
| 10,000 | 16 | 4,607,216 | 700,632 |
| 10,000 | 128 | 5,727,216 | 1,839,320 |
| 10,000 | 1,024 | 14,687,216 | 10,868,303 |


## Concurrent Runtime Comparison

32 persistent connections, one/two/four workers, both Weave schedulers with
sharded I/O. libpq uses one nonblocking poll/WSAPoll loop per worker, fixed
round-robin session assignment and no Weave runtime. SQL, parameter types,
checksums and protection policy match. Seven sequential samples alternate all
order permutations; prepared setup and 32 warmups are untimed.

The same native Windows hardware/provider versions above apply. Two fixed
physical database cores (including SMT siblings) are separate from client
workers, each on one logical CPU of a distinct physical core.
Throughput counts queries/s, or statements/s for a 32-command one-Sync batch.
Concurrent batch P99 covers the **whole batch**, unlike serial amortized P99.
Row-heavy queries return 128 integers plus 256-byte text each.

### Four Workers

Throughput is ops/s (statements/s for batches). A = affine, S = work stealing.
Spread lists A / S / PQ. P99 is a complete query or complete batch through Sync,
not the amortized serial batch latency. All four-worker throughput spreads are
below 10%; P99 comparisons marked noisy remain inconclusive independently.

| Transport | Workload | A | S | libpq | A / S vs PQ | Spread % A / S / PQ |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Plain | Simple | 105,239 | 108,303 | 114,188 | 0.92 / 0.94x | 4.8 / 2.9 / 2.5 |
| Plain | Prepared | 113,959 | 112,678 | 123,078 | 0.93 / 0.92x | 3.1 / 4.4 / 2.8 |
| Plain | Batch | 528,713 | 527,864 | 535,137 | 0.99 / 1.00x | 1.9 / 3.4 / 5.0 |
| Plain | Rows | 27,897 | 27,473 | 33,266 | 0.84 / 0.83x | 7.9 / 4.8 / 3.0 |
| TLS | Simple | 91,606 | 92,526 | 95,791 | 0.95 / 0.97x | 3.2 / 3.5 / 2.3 |
| TLS | Prepared | 97,302 | 97,481 | 102,172 | 0.95 / 0.95x | 5.0 / 3.2 / 3.0 |
| TLS | Batch | 470,737 | 468,698 | 470,659 | 1.00 / 0.99x | 5.9 / 7.5 / 4.2 |
| TLS | Rows | 11,214 | 11,233 | 11,309 | 1.00 / 1.00x | 2.3 / 2.7 / 2.0 |

| Transport | Workload | P99 us A / S / PQ | Comparable A / S to PQ |
| --- | --- | ---: | --- |
| Plain | Simple | 403.8 / 438.1 / 382.9 | No / No: baseline noisy |
| Plain | Prepared | 367.1 / 496.3 / 335.2 | Yes / No: S noisy |
| Plain | Batch | 2523.5 / 3455.3 / 6808.6 | No / No: both candidates noisy |
| Plain | Rows | 1912.0 / 5572.0 / 1351.2 | No / No: baseline noisy |
| TLS | Simple | 484.1 / 480.4 / 421.1 | Yes / No: S noisy |
| TLS | Prepared | 437.0 / 482.7 / 425.5 | No / No: baseline noisy |
| TLS | Batch | 2796.1 / 5887.6 / 9347.3 | No / No: all noisy |
| TLS | Rows | 16481.5 / 16553.2 / 21627.2 | No / No: all noisy |


CPU time is unavailable for ranking; raw cycles are not utilization.
The fixed database budget can saturate. These are closed-loop client comparisons,
not unconstrained database/client scaling, durable WAL throughput or a uniform
Weave win. P99 spread above 30% is inconclusive independently of throughput.
Throughput spread above 10%, or windows shorter than 75% of their declared target,
are also inconclusive for the affected metric. Keep every sample and failure.

## Reproduce

Build with `WEAVE_POSTGRES_BENCHMARKS=ON`, a private libpq 18+ development
dependency, and `WEAVE_MODULES="postgres;runtime"` for concurrent measurements.
Keep general benchmark suites off when only PG is needed. Use a dedicated test
database/login and set `WEAVE_PG_PASSWORD`; never commit credentials.

```text
python modules/postgres/benchmarks/run.py --executable <Release benchmark> \
  --host 127.0.0.1 --port 55432 --ca <trusted CA file or plain> \
  --seconds 2 --repetitions 7 --output <new ignored path>

python modules/postgres/benchmarks/run_concurrent.py \
  --executable <Release weave_postgres_concurrent> \
  --workers 1 2 4 --connections 32 --reserve-cores 2 \
  --seconds 2 --repetitions 7 --ca <trusted CA file or plain> \
  --server-description <actual placement> --output <new ignored path>
```

The runner's core reservation constrains client placement, not database affinity.
Document server placement separately. Connect includes authentication/finish;
Weave reuses credentials while libpq constructs TLS state per public connect.
COPY inserts 1,000 text rows into a connection-private temporary table;
large-object reads/overwrite/append transfer 64 KiB, without durable commits.
These API/setup/storage differences must not be attributed solely to IOCP/io_uring.

Drivers refuse evidence overwrite and validate consumed values/checksums.
Source changes make a run diagnostic-only. Native performance suites are manual;
correctness CI tests only their synthetic analyzer/process controls.
Pooling was added later and is **not measured by these results**.

### Borrowed Inputs And COPY Buffers

The separate manual `modules/postgres/benchmarks/buffers.py` comparison measures
owning/borrowed extended, prepared and binary queries plus COPY output against
libpq on an owned Windows cluster. It reserves two server cores and one separate
client core, rotates sequential execution order and checks consumed-data hashes.
Supply `--executable`, `--topology-executable` (the concurrent benchmark),
`--server-bin`, a new ignored `--output` path, and optionally `--before` for a
saved pre-change executable. Defaults are seven repetitions and a one-second
sample target: warmup precedes larger pilots, then all variants share a fixed
count targeting the fastest pilot plus 10%. Samples below 80% of the target are
marked short, never selectively retried. COPY output has
no pre-change timing cell unless that executable supported the workload.

Separate instrumented runs report driving-thread C++ `new` requests and requested
bytes per operation. These exclude C/DLL allocators, OpenSSL/ICU/libpq internals,
allocator-reserved memory and RSS. They are not total allocation counts or used
for throughput ranking. CPU readings retain the existing Windows resolution
limitation. These results must not be mixed with the older tables above.

Local Windows Release measurements on 2026-10-11 used a Ryzen 9 9900X,
PostgreSQL 16.4, libpq 18.4, plaintext TCP and one Context. Seven sequential
samples per variant retained all outliers. **Timing comparisons are inconclusive**:
rate spreads were 4.6-274.6%; the prepared workload also flagged a shortest sample
of 0.743 seconds. The apparent binary-query speedup is noise, not a claimed win.

| Workload | Before ops/s | Borrowed ops/s | Raw change | libpq ops/s | Spread before / borrowed / libpq |
| --- | ---: | ---: | ---: | ---: | --- |
| Extended | 8,786 | 8,719 | -0.8% | 9,473 | 162.6% / 187.7% / 162.2% |
| Prepared | 26,862 | 27,137 | +1.0% | 27,858 | 31.3% / 6.9% / 6.0% |
| Binary | 6,662 | 24,117 | +262.0% | 25,053 | 274.6% / 74.2% / 75.2% |
| COPY output | unavailable | 921 | unavailable | 978 | unavailable / 37.4% / 4.6% |

Separate 256-operation passes confirmed fewer C++ allocation requests:

| Workload | Current owning calls/op | Borrowed calls/op | Reduction | Requested bytes/op owning / borrowed |
| --- | ---: | ---: | ---: | ---: |
| Extended | 43 | 40 | 7.0% | 7,684 / 6,548 |
| Prepared | 36 | 34 | 5.6% | 7,507 / 6,403 |
| Binary | 43 | 40 | 7.0% | 7,686 / 6,550 |
| COPY output (128 rows of 4,097 bytes) | 403 | 275 | 31.8% | 982,708 / 434,724 |

Unpublished local evidence is `benchmarks/results/buffers-duration-20261011.json`,
SHA-256 `f9c0940259be02ad3c598398ad30d1b311391ada62d6fb7fb7fcabcbf908d7b4`.
Earlier short-pilot diagnostic runs remain alongside it, not overwritten or merged.
Allocation savings are not proof of throughput gains or a performance-parity gate.

## Provenance

[Complete raw evidence](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261010-postgres-final)
retains pilots, samples, source/binary hashes, placement and logs.
Asset SHA-256: `051b66ba58b15520bcb19b3c622c740bbfa4d7b843b5813cad876225b929c77c`.

Measured code was an **uncommitted working tree** based on
`258683c061d6b66cf55cdefe042959fc10770e30`, not the release tag's checkout.
Initial inputs `61fc3dfc...` accidentally double-reserved client cores and
omitted four-worker cells. Corrected inputs `2a617a7c...` measured only those
missing cells; identical production binaries were used. Existing one/two-worker
samples were preserved, not replaced for favorable results.

[Earlier Linux and WSL-forwarded baselines](https://github.com/c-schembri/weave/blob/4f24d614b31708b10007072b2b9e6aa323249bad/docs/postgres-benchmarks.md)
and [earlier concurrent tables](https://github.com/c-schembri/weave/blob/4f24d614b31708b10007072b2b9e6aa323249bad/docs/postgres-concurrency.md)
remain in Git history. They have different placement/source provenance and must
not be silently merged with the native Windows run.
