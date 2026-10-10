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

The [32-connection 1/2/4-worker report](postgres-concurrency.md#latest-native-windows-run-2026-10-10)
contains the Runtime comparison. [Complete raw evidence](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261010-postgres-final)
retains every pilot/sample, checksum, source/binary hash, placement and log.
Measured code is the **uncommitted working tree** based on
`258683c061d6b66cf55cdefe042959fc10770e30`, not the release tag's source checkout.
Frozen gate inputs are `61fc3dfc...` (initial) and `2a617a7c...` (missing four-worker
cells); production benchmark binaries are identical across both.

## Historical Baseline (2026-10-07)

Measured on 7 October 2026. This is the first native PostgreSQL implementation;
**before and before/after change are unavailable**, not zero. These results are
not Weave TCP/Tokio/Asio results and do not establish production qualification.

This page covers serial workloads. See the
[32-connection Runtime/libpq comparison](postgres-concurrency.md) for
1/2/4-worker measurements, CPU costs, tail latency and placement limits.

## Linux

Release GCC 15.2, Ubuntu 26.04 under WSL2, kernel 6.18.40.1, PostgreSQL/libpq
18.6, OpenSSL 3.5.5 with distribution security patches, ICU 78.2. The host is a
Ryzen 9 9900X (12 physical / 24 logical CPUs); this WSL VM exposes 8 logical CPUs.
One client connection and caller-thread Context, not Runtime scaling. The server
runs in the same VM. No affinity or exclusive machine reservation was imposed;
an unrelated user service remained running.

Each implementation ran sequentially, with seven paired samples and alternating
order. Retained pilots calibrated a common iteration count to approximately one
second for the slower implementation. All results were consumed and checksums
matched for every pair. Tables show median throughput, median sample P99, and
median client-process CPU time / measured wall time. Spread is
`(maximum - minimum) / median` throughput across all seven samples, not a confidence
interval. CPU excludes the database server and WSL host/forwarding processes.

### Plaintext SCRAM

| Workload | Weave ops/s | libpq ops/s | Weave / libpq | Spread W / PQ | P99 us W / PQ | CPU % W / PQ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Connect/authenticate/finish | 555 | 334 | 1.66x | 1.6 / 1.6% | 2290.5 / 3617.8 | 35.1 / 57.4 |
| Simple SELECT | 9,881 | 10,467 | 0.94x | 3.3 / 3.1% | 180.7 / 169.1 | 45.5 / 38.0 |
| Extended SELECT | 8,919 | 9,491 | 0.94x | 1.8 / 1.4% | 201.2 / 187.0 | 42.7 / 35.1 |
| Prepared SELECT | 10,713 | 11,526 | 0.93x | 1.9 / 2.1% | 164.4 / 150.8 | 50.3 / 42.4 |
| Binary result | 8,933 | 9,518 | 0.94x | 0.7 / 1.0% | 197.6 / 190.9 | 42.7 / 35.6 |
| Batch, 32 statements | 136,219 | 149,222 | 0.91x | 1.8 / 3.5% | 12.9 / 12.4 | 28.1 / 20.2 |
| Read 1,000 rows | 1,657 | 1,262 | 1.31x | 7.8 / 9.1% | 1152.0 / 1292.6 | 83.7 / 66.4 |
| COPY IN, 1,000 rows | 1,179 | 1,195 | 0.99x | 4.3 / 2.1% | 1276.9 / 1201.5 | 11.9 / 9.7 |

### Verified TLS + SCRAM-PLUS

| Workload | Weave ops/s | libpq ops/s | Weave / libpq | Spread W / PQ | P99 us W / PQ | CPU % W / PQ |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Connect/authenticate/finish | 285 | 210 | 1.36x | 0.9 / 3.2% | 4199.0 / 5478.0 | 32.8 / 49.3 |
| Simple SELECT | 8,435 | 9,078 | 0.93x | 2.5 / 1.6% | 205.8 / 195.6 | 51.1 / 40.3 |
| Extended SELECT | 7,655 | 8,347 | 0.92x | 2.1 / 2.0% | 228.3 / 211.7 | 48.4 / 37.5 |
| Prepared SELECT | 8,954 | 9,902 | 0.90x | 1.1 / 3.9% | 192.5 / 178.5 | 55.9 / 44.2 |
| Binary result | 7,714 | 8,327 | 0.93x | 2.0 / 2.7% | 228.1 / 213.0 | 48.7 / 37.6 |
| Batch, 32 statements | 125,490 | 138,784 | 0.90x | 1.1 / 2.8% | 13.6 / 13.1 | 31.8 / 22.6 |
| Read 1,000 rows | 1,467 | 1,131 | 1.30x | 3.0 / 10.1% | 1253.0 / 1464.2 | 85.4 / 67.6 |
| COPY IN, 1,000 rows | 1,135 | 1,149 | 0.99x | 2.1 / 1.9% | 1267.8 / 1299.5 | 15.9 / 11.5 |

Ordinary queries are slower and consume more client CPU than libpq in this
baseline. Connect and large row reads are faster; the row comparison has wider
spread, especially libpq with TLS. No uniform performance advantage is claimed.

## Windows Clients

Native MSVC 19.44 Release clients, OpenSSL 3.6.5, ICU 78.3, libpq 18.4 from the
pinned vcpkg snapshot. The database remains PostgreSQL 18.6 in WSL, reached via
Windows localhost forwarding. This is **not a native Windows database-server
benchmark** and must not be compared directly to Linux throughput as a backend
performance claim. Same seven-pair protocol, SQL, consumption and TLS policy.

### Plaintext SCRAM

| Workload | Weave ops/s | libpq ops/s | Weave / libpq | Spread W / PQ | P99 us W / PQ |
| --- | ---: | ---: | ---: | ---: | ---: |
| Connect/authenticate/finish | 265 | 66 | 4.03x | 5.9 / 6.9% | 4706.6 / 26390.9 |
| Simple SELECT | 4,555 | 4,622 | 0.99x | 3.1 / 2.5% | 318.3 / 318.7 |
| Extended SELECT | 4,225 | 4,358 | 0.97x | 3.0 / 5.6% | 346.6 / 332.2 |
| Prepared SELECT | 4,663 | 4,809 | 0.97x | 3.3 / 3.0% | 312.8 / 294.6 |
| Binary result | 4,264 | 4,423 | 0.96x | 6.2 / 1.9% | 335.4 / 332.8 |
| Batch, 32 statements | 82,430 | 93,830 | 0.88x | 4.7 / 3.1% | 17.4 / 16.2 |
| Read 1,000 rows, inconclusive | 687 | 910 | 0.75x | 60.1 / 57.3% | 1260.9 / 1004.2 |
| COPY IN, 1,000 rows | 23 | 23 | 1.00x | 0.8 / 0.8% | 44362.9 / 44402.1 |

### Verified TLS + SCRAM-PLUS

| Workload | Weave ops/s | libpq ops/s | Weave / libpq | Spread W / PQ | P99 us W / PQ |
| --- | ---: | ---: | ---: | ---: | ---: |
| Connect/authenticate/finish | 21 | 16 | 1.28x | 3.7 / 4.4% | 51429.7 / 71781.5 |
| Simple SELECT | 4,058 | 4,214 | 0.96x | 3.0 / 3.0% | 345.1 / 333.3 |
| Extended SELECT | 3,928 | 4,015 | 0.98x | 2.5 / 4.2% | 353.7 / 358.9 |
| Prepared SELECT | 4,286 | 4,453 | 0.96x | 3.9 / 4.3% | 334.7 / 328.0 |
| Binary result | 3,892 | 4,026 | 0.97x | 2.3 / 2.7% | 364.3 / 352.3 |
| Batch, 32 statements | 78,118 | 89,617 | 0.87x | 2.7 / 4.1% | 18.5 / 16.6 |
| Read 1,000 rows, inconclusive | 403 | 621 | 0.65x | 86.7 / 83.6% | 1533.1 / 1271.8 |
| COPY IN, 1,000 rows | 23 | 23 | 1.00x | 0.3 / 0.8% | 44330.4 / 44351.6 |

Windows row throughput is too variable to rank the clients. Approximately 44 ms
COPY P99 and much slower connection setup are observations of this cross-OS
path; their cause has not been isolated. Neither is attributed to a particular
library or used as an IOCP advantage claim. Windows GetProcessTimes readings
often reported zero over these samples. Subsequent concurrent-workload controls
confirmed underreporting with independent process sampling; the cause is not
isolated. Raw CPU measurements are retained, but cannot establish zero CPU cost
or rank CPU efficiency. The concurrent comparison reports raw cycles separately.

## Large Objects

Seven matched pairs per workload, calibrated to two seconds rather than one.
Same hardware, builds and transport policy as above. One operation seeks then
transfers 64 KiB through native FunctionCall messages. Both libraries reuse a
caller-owned receive buffer; reads verify and checksum every byte. Setup,
catalog lookup, final content verification and rollback are outside timing.
Before/change remain unavailable for this new module.

| Client / transport | Workload | Weave ops/s | libpq ops/s | W / PQ | Spread W / PQ | P99 us W / PQ |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Linux / plaintext | Read | 3,590 | 4,124 | 0.87x | 1.9 / 2.2% | 457.9 / 403.1 |
| Linux / plaintext | Overwrite | 109 | 112 | 0.98x | 1.6 / 3.5% | 19319.5 / 19100.0 |
| Linux / plaintext | Append, inconclusive | 2,003 | 2,103 | 0.95x | 36.7 / 8.9% | 762.8 / 709.6 |
| Linux / TLS | Read | 3,082 | 3,345 | 0.92x | 3.2 / 2.8% | 623.4 / 627.1 |
| Linux / TLS | Append, inconclusive | 1,576 | 2,040 | 0.77x | 19.1 / 51.4% | 1079.7 / 798.7 |
| Windows / plaintext | Read | 1,903 | 2,011 | 0.95x | 5.4 / 2.8% | 646.1 / 641.1 |
| Windows / plaintext | Append | 23 | 23 | 1.00x | 0.8 / 2.1% | 45125.3 / 44544.0 |
| Windows / TLS | Read | 1,642 | 1,853 | 0.89x | 2.2 / 3.2% | 740.3 / 660.5 |
| Windows / TLS | Append | 23 | 23 | 1.00x | 2.7 / 2.7% | 44416.6 / 44504.4 |

Reads are slower than libpq in these runs. Linux append throughput is too
variable to rank. Native Windows clients again encounter approximately 44 ms
write latency on the WSL-forwarded path; the cause is not isolated. No samples
were discarded or selectively rerun. Windows CPU time remains unreliable.

Linux client CPU percentages W / PQ: plaintext read 59.6 / 55.4, overwrite
2.5 / 1.2, append 31.6 / 17.5; TLS read 65.7 / 57.7, append 50.4 / 28.6.
These exclude server CPU and do not establish server capacity or durability.

`lo_write` repeatedly overwrites the same object pages in one transaction.
Increasing write cost made the pilot underestimate measured durations: the
completed plaintext sweep took substantially longer than two seconds per sample.
Its full results are retained, not replaced by the append numbers. `lo_append`
seeks to successive 64 KiB offsets, bounded to 8,192 operations / 512 MiB per
sample. Both are uncommitted writes followed by rollback, not durable disk
throughput. Only the plaintext Linux overwrite sweep was run; the other sweeps
measure read and append.

## Workload boundaries

- Connect includes startup, authentication and graceful finish. Weave reuses an
  immutable TLS credential snapshot; libpq's public connect API constructs its
  TLS state per connection. This measures those APIs' setup costs, not just IOCP
  or io_uring performance.
- Ordinary queries use `SELECT 42` or a bound integer parameter plus one. Prepared
  setup and ordinary connection setup are outside the measured loop. Binary mode
  changes result representation, not the SQL or consumed values.
- Batch uses 32 extended statements and one final Sync on both clients. Throughput
  is statements/s. Batch P99 is **amortized batch time per statement**, not the
  latency distribution of individual pipelined statements.
- Rows returns 1,000 integers and 256-byte payloads per query. Throughput counts
  queries/s, not rows/s. Both clients retain and consume complete results.
- COPY sends 1,000 text rows to a connection-private temporary table per operation.
  Table creation is outside timing; the table grows identically during a sample.
  Throughput counts COPY operations/s, not rows/s.
- These are serial baselines. They do not measure concurrent clients, runtime
  worker scaling, cancellation storms, slow peers or database durability under load.

## Reproduce

Build with `WEAVE_POSTGRES_BENCHMARKS=ON` and a private libpq development
dependency. Set `WEAVE_PG_PASSWORD` in the environment. Use a dedicated test
database and run outside builds/tests and correctness CI:

```text
python modules/postgres/benchmarks/run.py --executable <Release benchmark> \
  --host 127.0.0.1 --port 55432 --ca <trusted CA file or plain> \
  --seconds 1 --repetitions 7 --output <new ignored evidence path>
```

Use `--workloads lo_read lo_append --seconds 2` for the bounded large-object
comparison. Select `lo_write` explicitly for the slower overwrite workload.

No samples or outliers are discarded. The driver refuses to overwrite evidence,
retains failures and validates consumed-result checksums. Source changes during
measurement make the run diagnostic-only. Longer samples and a native Windows
server are needed before relying on the noisy Windows cases.

## Provenance

The measured source was uncommitted on top of `258683c`. Subsequent parity work
is not silently included in this baseline. Source fingerprint:
`ddab3f5051a07067c518ba953591fdf161415d82419ed25bee75d8dea0e520cf` on Linux,
`0a62a1432c17dbd72ab3a214fa04361cf28ab0be72a629862eaf1ed65f698a8c` on Windows.
The original driver sorted native Path objects, whose ordering differs by OS;
both runs read the same shared source tree and validated their own start/end
fingerprint. A local source/build-input snapshot is retained as
`postgres-source-20261007.zip`, SHA-256
`6678ae667908b30cb5f42d06dfe29bcd1b51d98f93ff480371ed597d0bdce499`.
Raw evidence is ignored local output, not tracked Git data or uploaded release
assets. Each completed sweep retains 128 records (16 pilots, 112 samples).

| Evidence | SHA-256 |
| --- | --- |
| postgres-linux-plain-20261007.json | 04551d571d8f36290646ad90db2ec66ece7e29ac185cbadb44e205095e0199af |
| postgres-linux-tls-20261007.json | 7d6db6862a7e7ea488e6a86150055325dc0b092182616c7913e8b713126689e3 |
| postgres-windows-plain-20261007.json | 2d5f377908b3c4412c3c358d437ecbccb14f70163ca2911d6673e3031d8ce783 |
| postgres-windows-tls-20261007.json | 1bd3431f4f016e9f9826cd071517e58ed73891aed2ab93873bd53d39e2cfa7e2 |

Large-object source fingerprint was
`d0e4a946c085541890deb945730617f993ee1304c71fe830c7699ebd3043ef85`
for the first read/overwrite sweep. Adding the bounded append workload changed
only the comparison/harness sources, yielding the common Windows/Linux fingerprint
`abd135cfe838aed07f9c5733df9c95b3de6e680e7efc95cd2ed5df8eabc62aa7`.
All five sweeps completed with matching checksums and unchanged start/end source.
Each JSON records its executable hash. Local source snapshots:

| Snapshot / evidence | SHA-256 |
| --- | --- |
| postgres-large-objects-source-20261007.zip | 1ee258b0ca1b149fa84e14406287dc3a99cab22064175e028640a86787058b3b |
| postgres-lo-append-source-20261007.zip | b511d9de1e49509945a97ebc46da371e3fe3a474180f3c3bae0b804994bd56ac |
| postgres-lo-linux-plain-20261007.json | 70ee4ca41154c2bb84aad0ac549117527c04cc2bb19f3025b4f11db586cfdd62 |
| postgres-lo-linux-append-20261007.json | 79e594e547b092587e636679196cbb181849ada747122e4928b0322f5220039f |
| postgres-lo-linux-tls-20261007.json | 299c0487379b6ca0c46a051dd64c28d400a13803c15d36bc82bedf151d2bdf52 |
| postgres-lo-windows-plain-20261007.json | f91455a7289db8cd3825d321ed6c98070460d3dec65efa0718c08ae7c27c0149 |
| postgres-lo-windows-tls-20261007.json | 3fef62db8c03acb46b03f67c82954e1cf886125af280fbaa11779decf3d1308b |
