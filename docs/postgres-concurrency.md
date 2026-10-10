# PostgreSQL Concurrent Baseline

## Latest Native Windows Run (2026-10-10)

32 connections, native PostgreSQL 18.6, libpq 18.4, MSVC 19.44 Release,
OpenSSL 3.6.5, Ryzen 9 9900X. Two physical database cores (including SMT siblings)
are separate from the client; each worker uses one logical CPU from a distinct
physical client core. Both schedulers use sharded I/O. Seven sequential samples
per client use all order permutations and common counts targeting two seconds
for the fastest pilot. Matching SQL, result checksums, verified TLS 1.3/SCRAM-PLUS
and plaintext SCRAM policies are pinned. No before/after optimization is claimed.

The initial runner erroneously reserved two cores again inside its already
restricted four-core client mask, so it measured only one/two workers. Those
samples are retained. The corrected runner measured **only the missing four-worker
cells**, all workloads and all three clients, in both transport modes. It did not
replace slow/noisy samples or rerun favorable results. Both raw campaigns share
identical benchmark binaries; their source snapshots differ in runner arguments.

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

Windows CPU-time readings are unreliable and unavailable. Raw process cycles
remain in JSON, with their own noise labels; they are not CPU utilization.
The database's fixed two-core budget can saturate, especially in TLS row runs:
this does not establish unconstrained client scaling or equal efficiency.

[All one/two/four-worker samples and summaries](https://github.com/c-schembri/weave/releases/tag/benchmarks-20261010-postgres-final),
[serial and retained-memory report](postgres-benchmarks.md),
[working-tree provenance and validation](postgres-closure.md).

## Historical Baseline (2026-10-07)

Measured on 7 October 2026: **32 simultaneous connections**, 1/2/4 client
workers where available, against PostgreSQL 18.6. This extends the
[serial baseline](postgres-benchmarks.md); it is not a before/after optimization.
**Before and percentage change are unavailable.** The matched baseline is libpq,
not Asio/Tokio. No production-readiness or uniform performance advantage is claimed.

## Method

Three client implementations run **sequentially**, outside builds, tests and CI:

- **A**: Weave Runtime with worker-affine scheduling.
- **S**: Weave Runtime with work stealing. Roots use ordinary round-robin
  `spawn`, not pinned `spawn_on`; sockets retain their original Context.
- **PQ**: libpq's nonblocking send/flush/consume/busy/result APIs, with one
  `poll`/WSAPoll loop per worker and a fixed, round-robin session assignment.
  No thread per connection or idle Weave Runtime in the libpq path.

Every connection completes setup, prepared-statement setup where applicable,
and 32 warmup requests before the measurement gate. Both clients use protocol
3.0, require SCRAM, disable GSS encryption, and use TCP_NODELAY. TLS runs require
TLS 1.3, certificate/name verification and SCRAM-PLUS; no client certificate.

Retained pilots calibrate one common per-connection iteration count for all
three implementations, targeting three seconds for the fastest pilot.
Seven samples per client/configuration use all six order permutations, then
repeat the first order. Count, SQL, parameter types and consumed values match.
Every value is independently checked and checksummed; all measured and pilot
comparisons matched. Setup/warmup, final disconnect and percentile sorting are
outside timing. Dispatch, protocol work, allocations and value consumption are
inside. Fixed-count sessions can finish at different times; this is closed-loop
client workload throughput, not maximum sustained database capacity.

Workloads:

- Simple: `SELECT 42::int`, one query per connection in flight.
- Prepared: named `SELECT $1::int + 1`, parameter 41, prepared before timing.
- Batch: 32 extended statements and one Sync, identical SQL/parameter, no
  intermediate synchronization. Throughput counts **statements/s**.
- Rows: 128 integers and 256-byte text payloads per query. Both retain and
  consume the complete result. Throughput counts **queries/s**, not rows/s.

Latency covers one complete request, including value consumption, or the
**whole 32-statement batch through Sync**, not an amortized statement latency.
P99 is the median of seven sample P99s. CPU counters cover all client-process
threads, including the driver; database/host-forwarding CPU is excluded.

Spread is `(max - min) / median` across all seven samples. No outliers or
failures are discarded or selectively retried. A throughput spread over 10%,
P99 spread over 30%, or measured duration below 75% of the target makes that
metric's comparison inconclusive. Comparisons check both clients' quality.
Raw evidence also retains CV, P50/P95 and deterministic paired bootstrap
intervals; seven samples do not establish a universal confidence guarantee.

## Placement And Limits

Host: Ryzen 9 9900X, 12 physical / 24 logical CPUs. Linux: Ubuntu 26.04 WSL2,
kernel 6.18.40.1, GCC 15.2 Release, OpenSSL 3.5.5 with distribution patches,
ICU 78.2 and libpq 18.6. Windows: native MSVC 19.44 Release, OpenSSL 3.6.5,
ICU 78.3 and libpq 18.4 from the pinned vcpkg build.

The owned WSL database master and its existing auxiliary processes were pinned
to guest CPUs 0 and 2 before the sweep; new backends inherit that placement.
Previous affinities were restored afterward. The VM reports four physical /
eight logical CPUs. Client processes use one logical CPU per other reported
physical core: CPU 6 for one worker, CPUs 4/6 for two.
**Four Linux workers are unavailable** after the fixed two-server-core reservation.

Native Windows clients use CPUs 22, 20/22, or 16/18/20/22, respectively.
They reach the same WSL database through Windows localhost forwarding.
VM-to-host CPU placement is unknown and unrelated services remain running:
guest separation is not certified host isolation. These Windows results are
**not a native Windows database-server benchmark**. Do not compare absolute
Windows/Linux numbers as an IOCP-versus-io_uring claim.

The Windows `GetProcessTimes` counter underreported sustained worker activity,
including zero process CPU time during a 5.25-second row workload. Independent
`Get-Process` sampling also reported zero, while a CPU-bound control accounted
for two CPU seconds and the worker workload recorded billions of cycles.
The cause is not isolated. All raw readings are retained; Windows CPU time and
utilization are **unavailable for ranking**, not zero-cost.

Windows tables instead show raw process cycles/op from
[QueryProcessCycleTime](https://learn.microsoft.com/en-us/windows/win32/api/realtimeapiset/nf-realtimeapiset-queryprocesscycletime).
They are not converted to time, utilization, or energy; clock-frequency effects
remain. Linux tables show process CPU time/op and utilization, which may exceed
100% with multiple workers.

## Results

Triples are **A / S / PQ**. Ratios are medians of within-repetition Weave/PQ
throughput ratios, not ratios of the displayed medians. An asterisk marks an
inconclusive comparison for that metric; values remain visible.

### Linux / Plaintext SCRAM

| Workers | Workload | A ops/s | S ops/s | PQ ops/s | A/PQ | S/PQ | Spread % A/S/PQ |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | Simple SELECT | 97,861 | 95,219 | 152,076 | 0.65x | 0.63x | 1.5 / 2.2 / 3.1 |
| 1 | Prepared SELECT | 76,075 | 75,436 | 92,998 | 0.82x | 0.81x | 4.4 / 1.6 / 6.4 |
| 1 | Batch (32) | 425,997 | 428,258 | 401,826 | 1.05x | 1.06x | 4.5 / 7.6 / 6.3 |
| 1 | 128-row read | 17,441 | 17,501 | 17,321 | 1.01x | 1.01x | 2.5 / 2.9 / 2.5 |
| 2 | Simple SELECT | 80,791 | 78,430 | 66,850 | 1.21x | 1.17x | 5.4 / 1.2 / 7.0 |
| 2 | Prepared SELECT | 121,368 | 65,635 | 100,768 | 1.19x* | 0.65x* | 7.8 / 1.9 / 14.4 |
| 2 | Batch (32) | 383,064 | 447,933 | 376,112 | 1.03x | 1.20x | 4.3 / 3.4 / 4.3 |
| 2 | 128-row read | 31,567 | 25,796 | 39,509 | 0.80x | 0.65x | 7.2 / 1.3 / 5.2 |

| Workers | Workload | P99 us A/S/PQ | P99 spread % A/S/PQ | CPU % A/S/PQ | CPU us/op A/S/PQ |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | Simple SELECT | 554.5 / 555.2 / 549.4 | 5.4 / 6.3 / 20.4 | 99.8 / 99.8 / 98.4 | 10.20 / 10.47 / 6.51 |
| 1 | Prepared SELECT | 770.6 / 776.7 / 877.5 | 29.5 / 26.1 / 12.0 | 99.9 / 99.9 / 99.3 | 13.13 / 13.24 / 10.69 |
| 1 | Batch (32) | 6,657.4 / 6,646.1 / 6,685.2 | 10.0 / 13.8 / 10.2 | 66.9 / 69.9 / 36.0 | 1.58 / 1.63 / 0.90 |
| 1 | 128-row read | 2,251.6 / 2,235.0 / 2,605.6 | 9.6 / 8.6 / 4.0 | 100.0 / 100.0 / 100.0 | 57.33 / 57.14 / 57.66 |
| 2 | Simple SELECT | 1,073.9 / 579.0 / 1,295.8 | 8.9 / 9.0 / 9.4 | 120.7 / 109.9 / 100.5 | 15.02 / 14.01 / 14.90 |
| 2 | Prepared SELECT | 760.8 / 752.1 / 891.8 | 7.4 / 22.4 / 13.4 | 147.1 / 109.2 / 122.5 | 12.12 / 16.62 / 12.07 |
| 2 | Batch (32) | 7,084.7 / 6,278.3 / 6,613.1 | 9.1 / 10.0 / 11.3 | 67.6 / 108.4 / 38.1 | 1.76 / 2.42 / 1.00 |
| 2 | 128-row read | 4,952.5* / 1,584.8 / 2,278.0 | 63.5 / 11.3 / 8.6 | 186.1 / 158.6 / 180.3 | 59.19 / 61.33 / 45.49 |

### Linux / Verified TLS + SCRAM-PLUS

| Workers | Workload | A ops/s | S ops/s | PQ ops/s | A/PQ | S/PQ | Spread % A/S/PQ |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | Simple SELECT | 73,258 | 72,924 | 110,751 | 0.66x | 0.66x | 1.0 / 1.7 / 3.9 |
| 1 | Prepared SELECT | 69,473 | 68,838 | 64,604 | 1.08x* | 1.07x* | 2.2 / 0.9 / 15.1 |
| 1 | Batch (32) | 421,582 | 427,051 | 389,365 | 1.09x | 1.10x | 4.4 / 3.2 / 5.5 |
| 1 | 128-row read | 13,327 | 13,283 | 13,767 | 0.97x | 0.97x | 2.9 / 0.7 / 1.4 |
| 2 | Simple SELECT | 77,615 | 69,032 | 56,863 | 1.36x | 1.20x | 2.3 / 1.7 / 9.3 |
| 2 | Prepared SELECT | 107,569 | 66,074 | 84,949 | 1.28x | 0.77x | 3.5 / 2.2 / 9.9 |
| 2 | Batch (32) | 368,205 | 452,639 | 358,407 | 1.03x | 1.26x* | 5.2 / 11.2 / 2.7 |
| 2 | 128-row read | 25,261 | 18,346 | 35,585 | 0.71x | 0.52x | 4.0 / 1.5 / 5.2 |

| Workers | Workload | P99 us A/S/PQ | P99 spread % A/S/PQ | CPU % A/S/PQ | CPU us/op A/S/PQ |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | Simple SELECT | 654.5 / 648.7 / 786.3 | 9.9 / 8.8 / 25.5 | 99.9 / 99.9 / 98.1 | 13.64 / 13.69 / 8.87 |
| 1 | Prepared SELECT | 624.4 / 624.1 / 1,266.7 | 14.5 / 4.3 / 21.3 | 100.0 / 100.0 / 99.5 | 14.39 / 14.52 / 15.43 |
| 1 | Batch (32) | 6,763.7 / 6,641.0 / 6,546.9 | 10.0 / 7.1 / 12.0 | 74.6 / 75.8 / 41.1 | 1.76 / 1.78 / 1.06 |
| 1 | 128-row read | 2,910.7 / 2,962.9 / 2,981.7 | 12.7 / 7.8 / 2.9 | 100.0 / 100.0 / 100.0 | 75.03 / 75.27 / 72.61 |
| 2 | Simple SELECT | 1,174.8 / 643.1 / 1,494.4 | 4.4 / 5.8 / 19.8 | 139.3 / 122.5 / 107.7 | 18.03 / 17.71 / 18.88 |
| 2 | Prepared SELECT | 876.6 / 670.9 / 1,062.2 | 5.4 / 7.3 / 12.9 | 160.6 / 122.4 / 127.5 | 14.83 / 18.55 / 15.12 |
| 2 | Batch (32) | 7,687.5 / 6,342.7 / 7,380.0 | 10.8 / 18.7 / 18.4 | 79.4 / 126.6 / 43.9 | 2.16 / 2.79 / 1.22 |
| 2 | 128-row read | 2,349.7* / 2,189.0 / 2,564.5 | 148.1 / 6.1 / 12.0 | 194.0 / 158.5 / 188.4 | 76.42 / 86.47 / 53.08 |

### Windows / Plaintext SCRAM

| Workers | Workload | A ops/s | S ops/s | PQ ops/s | A/PQ | S/PQ | Spread % A/S/PQ |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | Simple SELECT | 25,210 | 25,157 | 25,174 | 1.00x | 1.00x | 1.7 / 0.5 / 1.7 |
| 1 | Prepared SELECT | 28,295 | 28,275 | 28,486 | 1.00x | 0.99x | 1.8 / 2.0 / 2.3 |
| 1 | Batch (32) | 293,951 | 297,802 | 295,317 | 0.98x | 1.01x | 4.3 / 2.3 / 3.7 |
| 1 | 128-row read | 11,566 | 11,502 | 13,022 | 0.89x | 0.88x | 2.9 / 2.4 / 3.1 |
| 2 | Simple SELECT | 25,138 | 25,043 | 25,057 | 1.00x | 1.00x | 0.9 / 1.3 / 1.1 |
| 2 | Prepared SELECT | 28,289 | 28,286 | 28,354 | 1.00x | 1.00x | 0.9 / 1.8 / 1.6 |
| 2 | Batch (32) | 297,702 | 299,485 | 296,277 | 1.00x | 1.00x | 2.4 / 2.6 / 2.4 |
| 2 | 128-row read | 13,173 | 13,266 | 13,066 | 1.01x | 1.02x | 1.3 / 1.8 / 1.3 |
| 4 | Simple SELECT | 25,008 | 25,134 | 25,064 | 1.00x | 1.00x | 0.5 / 1.0 / 0.8 |
| 4 | Prepared SELECT | 28,309 | 28,233 | 28,293 | 1.00x | 1.00x | 1.8 / 1.9 / 1.1 |
| 4 | Batch (32) | 297,364 | 295,906 | 295,709 | 1.00x | 1.00x | 1.3 / 2.8 / 2.7 |
| 4 | 128-row read | 13,273 | 13,287 | 13,129 | 1.02x | 1.01x | 2.2 / 2.6 / 3.1 |

| Workers | Workload | P99 us A/S/PQ | P99 spread % A/S/PQ | Cycles/op A/S/PQ | Cycle spread % A/S/PQ |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | Simple SELECT | 2,971.1 / 2,913.9 / 2,798.5 | 13.7 / 9.5 / 19.0 | 92,756 / 94,839 / 110,111 | 2.3 / 1.1 / 3.2 |
| 1 | Prepared SELECT | 3,032.7 / 2,969.9 / 2,833.1 | 8.5 / 12.4 / 11.0 | 91,334 / 92,877 / 104,404 | 2.7 / 2.6 / 1.9 |
| 1 | Batch (32) | 10,120.8 / 9,866.4 / 10,233.8 | 25.8 / 26.1 / 16.8 | 9,747 / 9,723 / 5,332 | 1.6 / 1.4 / 3.4 |
| 1 | 128-row read | 3,645.0 / 3,698.7 / 6,954.9 | 24.2 / 27.6 / 13.1 | 377,434 / 380,010 / 308,083 | 2.2 / 1.6 / 1.9 |
| 2 | Simple SELECT | 2,964.6 / 2,860.8 / 2,855.1 | 12.2 / 13.6 / 10.1 | 96,509 / 101,742 / 105,611 | 1.1 / 1.0 / 1.3 |
| 2 | Prepared SELECT | 2,780.8 / 2,843.7 / 2,850.6 | 17.5 / 11.6 / 17.7 | 96,034 / 101,223 / 101,135 | 1.9 / 3.4 / 3.6 |
| 2 | Batch (32) | 9,531.4 / 9,768.9 / 9,311.8 | 29.2 / 23.7 / 23.4 | 10,355* / 10,409* / 5,220 | 15.6 / 16.8 / 2.2 |
| 2 | 128-row read | 6,428.0 / 6,362.3 / 6,544.7 | 6.1 / 5.7 / 16.3 | 405,247 / 422,758 / 325,903 | 2.8 / 2.0 / 1.8 |
| 4 | Simple SELECT | 2,830.1 / 3,057.6 / 3,071.4 | 16.6 / 14.8 / 16.3 | 101,200 / 106,015 / 104,396 | 1.1 / 1.2 / 1.3 |
| 4 | Prepared SELECT | 2,812.6 / 2,953.8 / 2,786.1 | 11.2 / 18.9 / 20.4 | 105,460 / 111,181 / 104,348 | 1.6 / 3.0 / 1.3 |
| 4 | Batch (32) | 9,634.4 / 8,849.9 / 9,108.4 | 16.0 / 17.2 / 18.0 | 10,571* / 11,101* / 5,013 | 14.3 / 13.5 / 1.9 |
| 4 | 128-row read | 6,333.9 / 6,381.6 / 6,403.2 | 11.1 / 10.5 / 14.4 | 443,225 / 455,566 / 331,183 | 2.2 / 2.2 / 2.2 |

### Windows / Verified TLS + SCRAM-PLUS

| Workers | Workload | A ops/s | S ops/s | PQ ops/s | A/PQ | S/PQ | Spread % A/S/PQ |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | Simple SELECT | 22,363 | 22,509 | 22,540 | 0.99x | 1.00x | 1.6 / 1.1 / 1.2 |
| 1 | Prepared SELECT | 25,288 | 25,084 | 25,164 | 1.00x | 1.00x | 1.3 / 2.3 / 1.7 |
| 1 | Batch (32) | 284,897 | 282,226 | 283,360 | 1.01x | 0.99x | 2.4 / 1.2 / 4.1 |
| 1 | 128-row read | 10,145 | 10,139 | 11,072 | 0.91x | 0.92x | 1.0 / 2.1 / 3.1 |
| 2 | Simple SELECT | 22,442 | 22,429 | 22,563 | 0.99x | 0.99x | 1.4 / 1.4 / 2.6 |
| 2 | Prepared SELECT | 25,211 | 25,156 | 25,282 | 1.00x | 1.00x | 2.3 / 1.4 / 0.8 |
| 2 | Batch (32) | 282,314 | 282,126 | 282,272 | 0.99x | 1.00x | 4.0 / 2.6 / 2.5 |
| 2 | 128-row read | 11,158 | 11,135 | 11,075 | 1.00x | 1.01x | 5.3 / 1.5 / 2.1 |
| 4 | Simple SELECT | 22,414 | 22,455 | 22,435 | 1.00x | 1.00x | 0.7 / 0.9 / 1.0 |
| 4 | Prepared SELECT | 25,167 | 25,104 | 25,132 | 1.00x | 1.00x | 1.6 / 1.2 / 2.3 |
| 4 | Batch (32) | 283,017 | 281,590 | 280,075 | 1.01x | 1.01x | 2.3 / 3.1 / 3.3 |
| 4 | 128-row read | 11,086 | 11,176 | 11,067 | 1.01x | 1.01x | 2.0 / 1.6 / 3.9 |

| Workers | Workload | P99 us A/S/PQ | P99 spread % A/S/PQ | Cycles/op A/S/PQ | Cycle spread % A/S/PQ |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | Simple SELECT | 3,255.4 / 3,155.4 / 3,122.0 | 8.8 / 15.5 / 23.5 | 106,278 / 107,850 / 123,040 | 1.6 / 2.2 / 3.4 |
| 1 | Prepared SELECT | 2,952.7 / 3,018.2 / 2,923.7 | 11.7 / 10.8 / 17.0 | 105,650 / 108,254 / 118,066 | 2.1 / 1.2 / 2.4 |
| 1 | Batch (32) | 10,052.8 / 10,380.8 / 9,990.5 | 16.1 / 28.5 / 15.3 | 10,488* / 10,616* / 5,662* | 5.4 / 4.3 / 15.7 |
| 1 | 128-row read | 5,748.8 / 5,778.2 / 7,756.2 | 1.6 / 2.7 / 15.2 | 431,770 / 431,815 / 379,074 | 1.0 / 1.9 / 3.5 |
| 2 | Simple SELECT | 3,416.5 / 3,128.9 / 3,021.2 | 16.7 / 9.3 / 6.1 | 111,248 / 117,000 / 118,940 | 2.6 / 1.3 / 1.8 |
| 2 | Prepared SELECT | 2,883.6 / 2,941.2 / 2,799.3 | 17.6 / 13.3 / 5.9 | 111,236 / 116,194 / 114,483 | 3.3 / 1.6 / 0.9 |
| 2 | Batch (32) | 10,552.2 / 9,172.3 / 10,040.2 | 20.3 / 25.1 / 9.4 | 10,858 / 10,783 / 5,559 | 3.9 / 5.5 / 7.4 |
| 2 | 128-row read | 7,464.7 / 7,687.7 / 7,454.2 | 11.5 / 6.6 / 11.2 | 475,243 / 492,119 / 411,745 | 2.8 / 3.9 / 2.8 |
| 4 | Simple SELECT | 3,138.2 / 3,142.0 / 3,010.6 | 11.0 / 13.0 / 9.3 | 116,688 / 121,984 / 116,877 | 1.0 / 2.0 / 0.8 |
| 4 | Prepared SELECT | 2,898.6 / 2,977.8 / 2,893.1 | 14.9 / 20.5 / 15.1 | 118,999 / 124,975 / 115,658 | 3.9 / 1.2 / 3.6 |
| 4 | Batch (32) | 8,802.7 / 9,943.5 / 10,161.5 | 28.7 / 21.2 / 19.0 | 11,607 / 11,931* / 5,381 | 6.5 / 20.7 / 2.1 |
| 4 | 128-row read | 7,820.5 / 7,808.0 / 7,464.2 | 11.2 / 8.0 / 23.4 | 515,827 / 533,822 / 414,388 | 1.9 / 2.8 / 1.7 |

## Interpretation

- Linux one-worker simple queries are slower than libpq: about 0.65x plaintext
  and 0.66x TLS. Two-worker simple queries favor Weave in this setup; this
  nonmonotonic behavior does not establish a general scaling curve or its cause.
- Two-worker prepared workloads show a material difference between Weave
  schedulers. The TLS affine comparison is stable at 1.28x libpq throughput;
  stealing is stable at 0.77x. The plaintext baseline is too noisy to rank.
- Two-worker row reads are slower than libpq for both schedulers. Affine P99
  is inconclusive, independently of stable throughput.
- Windows simple/prepared/batch throughput is near parity, and generally flat
  from one to four workers on this cross-OS path. One-worker row reads are
  slower; two/four-worker throughput is near parity. This is not evidence of
  maximum library/database capacity or a library-specific scaling limit.
- Weave usually spends more client CPU time or raw cycles per batch/row operation.
  Similar throughput does not imply equal CPU efficiency. Flagged cycle
  comparisons remain inconclusive; Windows utilization remains unqualified.

No networking implementation was optimized for these results. They provide a
baseline and expose workload/scheduler tradeoffs rather than justify promotion
to production or guarantee superiority to another client.

## Reproduce

Build locally with `WEAVE_MODULES="postgres;runtime"` and
`WEAVE_POSTGRES_BENCHMARKS=ON`; keep the general benchmark suite OFF on Linux.
The private comparison dependency is **libpq 18+**. Neither libpq nor Runtime
becomes a public dependency of the PostgreSQL module.

Set `WEAVE_PG_PASSWORD`, provide a dedicated server with the benchmark login
`weave` and database `postgres`, and use a trusted CA for TLS:

```text
python modules/postgres/benchmarks/run_concurrent.py \
  --executable <Release weave_postgres_concurrent> \
  --workers 1 2 4 --connections 32 --reserve-cores 2 \
  --seconds 3 --repetitions 7 --ca <CA file or plain> \
  --server-description <actual placement and isolation limits> \
  --output <new ignored evidence path>
```

The core reservation controls client eligibility/placement; it does **not**
move the database. Server placement must be controlled and documented separately.
A full sweep is deliberately supplemental and long, not a CI requirement.
For a narrower investigation select `--workers` and `--workloads` explicitly,
without presenting that subset as the complete matrix.

Correctness CI may run `weave_postgres_benchmark_tooling`, which uses synthetic
metrics and mocked processes only. It never launches the native benchmark,
database or performance measurements.

## Qualification And Provenance

Debug, Release and ASan builds passed on both platforms. Five PostgreSQL CTest
entries passed per Windows configuration and six per Linux configuration
(including disposable real-server tests). Eight synthetic harness tests cover
physical-core budgets, count/latency/CPU validation, metric-specific noise,
paired ratios, scheduling, environment isolation and retained failures.
Forty-eight additional short native checks (all clients/workloads in Debug and
ASan on both platforms) matched consumed results. PostgreSQL-only and
all-component packaging passed on both platforms.

Exploratory setup/test failures were retained and attributed: the ASan build
initially found an older system libpq, one local build had benchmarks disabled,
a Python 3.14 metadata lookup encountered the subprocess test mock, and WSL
stripped a packaging selector's quotes. Final corrected checks passed; none
were discarded/retried performance samples.

All four measured sweeps completed with **840 measured windows**, 186 retained
pilot/calibration windows, no failed measurements, matching results, and unchanged
source/executable fingerprints. Shortest measured windows were 2.39 seconds
(Linux plaintext), 2.34 (Linux TLS), 2.89 (Windows plaintext), 2.92 (Windows TLS);
all exceed the predeclared 2.25-second minimum.

Source was uncommitted on top of `258683c`, with 185 source/build/harness files
archived before measurement. Common source fingerprint:
`c38fabab4278772950a2b117b4c5c8f84823811e057760986b32f86bc5836d4f`.
Snapshot `postgres-concurrent-source-20261007.zip` SHA-256:
`23a2bb0249390fcfa5f8c8c239fed62571fe4c5fd8d5e79923441bff0a809a30`.

Release executable SHA-256:

- Linux: `a16fde5946676908d27634584f0a52425accd29df6d2cc390e88abe05a1029ce`.
- Windows: `d83d983c55f132e1f67a84f4b483dc5a9c84a54ab725cb829bdd2cc32306f150`.

Raw evidence and source snapshots remain ignored local output, not tracked Git
files or uploaded assets. Qualification logs are retained separately as
`postgres-concurrency-qualification-20261007.json`.

| Evidence | SHA-256 |
| --- | --- |
| postgres-concurrent-linux-plain-20261007.json | b18e19e4cf7cc3d2dd093c6c08c4b5aa9cf79a27a7cddac06b62f6ecc6b55d97 |
| postgres-concurrent-linux-tls-20261007.json | 1e4f615d0599461728579a9e809ba63633f610b0fb447e1c18d840f6f411c717 |
| postgres-concurrent-windows-plain-20261007.json | bda0a35245bddd64d2ad6f614b3a2f8dff88a96450969ffecdd6cfd7771101fc |
| postgres-concurrent-windows-tls-20261007.json | b49526af4dd05ac23dd521b981270f49a433cc1a1f7c042c05e2ce39987b1f8c |
