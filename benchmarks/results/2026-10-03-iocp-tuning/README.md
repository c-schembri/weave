# IOCP tuning experiment

Exploratory local measurements, 2026-10-03. Windows 11 build 22631,
Ryzen 9 9900X (12 cores / 24 logical processors), MSVC 19.44.35221,
VS 2022 x64 Release. Standalone Asio 1.36.0; Google Benchmark 1.9.4.
Both executables came from a dirty worktree based on b0dc9c8, not clean,
independently reproducible revisions. The original executable was retained
before runtime changes; the benchmark workloads and compiler settings match.

## Changes

- Enable skip-completion-on-success for supported IFS sockets, with queued
  fallback. Resume immediate successes inline instead of dequeuing them.
- Post a synthetic completion every 32 immediate successes to bound inline
  chaining. Preserve operation lifetime until that packet is consumed.
- Flatten read_exactly's receive loop, removing its nested read coroutine.
- Bound individual receive/send submissions to 64 KiB.

Smaller sends alone regressed performance (chunked.json); they are not a
standalone recommendation. These results measure the combined changes, not
the isolated contribution of each change. Public Async/Result usage is unchanged.

## Uninstrumented timing

Wall-clock means, milliseconds per complete concurrent-send/receive echo.
Lower is better. Setup is outside timing; joins and coroutine frames are inside.

| Run | Payload | Weave before | Weave after | Asio before | Asio after |
| --- | --- | ---: | ---: | ---: | ---: |
| All cases | 1 MiB | 0.394 | 0.349 | 0.513 | 0.442 |
| All cases | 8 MiB | 5.018 | 3.270 | 3.231 | 2.957 |
| Bulk repeat | 1 MiB | 0.390 | 0.326 | 0.407 | 0.380 |
| Bulk repeat | 8 MiB | 4.600 | 3.145 | 3.459 | 3.230 |

The all-case runs used 0.2s minimum, three repetitions, randomized case
interleaving, before then after. The bulk repeat used 0.3s minimum, five
repetitions, randomized interleaving, after then before. Raw iteration and
aggregate data are in final-before/after.json and repeat-before/after.json.

Weave's 8 MiB time decreased by 32-35% in these two comparisons. Unchanged
Asio also moved between runs, demonstrating host variability. Weave was slower
than Asio in one optimized 8 MiB run and slightly faster in the other: this is
NOT evidence of a reliable lead over Asio. Small echo latency remained roughly
unchanged. The coroutine-only case remains slower than Asio. These short local
runs are not a production throughput, fairness, or tail-latency claim.

## Instrumented work counts

profile-before.json and profile-final.json use WEAVE_PROFILE_RUNTIME=ON.
Do not compare their timings with uninstrumented Asio. Counts include amortized
root overhead and depend on receive fragmentation and scheduling.

| Per 8 MiB transfer | Before | After |
| --- | ---: | ---: |
| Coroutine allocation requests | ~65 | ~5 |
| Requested coroutine frame bytes | ~27,959 | ~1,397 |
| Queued completion packets | ~61 | ~14 |
| Receive submissions | ~60 | ~128 |
| Send submissions | 1 | 128 |
| Inline completions | 0 | ~242 |
| Synthetic fairness posts | 0 | ~7 |

This is fewer queue roundtrips and fewer frames, NOT fewer socket submissions.
Frames are still allocated; there is no pool. The send/receive cap changes how
the TCP stack and peer overlap work. Allocation counts cover coroutine frames
only, not all heap allocations.

## Verification and provenance

Debug and Release CTest passed, including every benchmark smoke case. Address
Sanitizer CTest passed. Added tests exercise both skip-enabled and forced-queued
modes, repeated client/server exchanges, cancellation, and large duplex
transfers with all packets drained. A dedicated starvation/tail-latency stress
test remains future work.

Executable SHA256 values (binaries retained locally under out/build, not Git):

```text
before C90ED5DC67E72D1168F7F3CADE870AE39CE1CDA9945F77B1B9DCA8CD7769165A
after  CF8E3D71C37EE6B9E483BEDADF4EA80C2BB54B5D712BEB022142FCEAEA3ACA2B
```

Re-run with the documented benchmark script on a quiet host before establishing
a performance gate. Preserve a clean source revision for the next baseline.
