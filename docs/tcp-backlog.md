# Windows TCP backlog investigation

## Confirmed defect

Weave and the comparison servers passed a plain positive `8192` to Winsock
`listen`. The provider silently limited the queue to 200 on this Windows 11 host.
Thus the benchmark configuration described an intended queue size, not the
effective native queue size.

An isolated native test opened 1024 clients without accepting any connections.
Across three repetitions, plain `8192` connected 200; `SOMAXCONN_HINT(8192)`
connected all 1024. During the earlier live-server probes, TCP table snapshots
showed hundreds of clients in `SYN_SENT` on both Weave and Asio, not solely
established connections awaiting coroutine resumption.

[Microsoft documents the encoded hint and provider-dependent backlog limits](https://learn.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-listen).
There is no standard API for querying the actual queue capacity. The observations
above establish this host's behavior, not a universal capacity for every provider.

## Fix And Regression

Weave translates explicit positive backlogs above 200 into `SOMAXCONN_HINT`,
capping the requested hint at 65535. `SOMAXCONN`, smaller values, and existing
native negative hints retain their previous handling. No scheduler, completion,
cancellation, socket-lifetime or task semantics changed.

The Asio comparison uses the same macro. Tokio 1.53.2 forwards its unsigned
backlog through an `i32` cast to Winsock, so its Windows comparison passes the
corresponding encoded `-8192` bit pattern. Non-Windows Tokio retains `8192`.

The permanent TCP regression opens 512 clients without accepting any, testing
requested backlogs 512 and 8192 with both completion modes. All four cases fail
with native `WSAECONNREFUSED` (10061) when the original plain-backlog call is
restored, and pass with the fix. Keeping the listener queue full prevents a fast
accept loop from masking the defect. The five-second cancellation/drain
boundary is a correctness guard, not a latency benchmark.

## Manual Evidence

All paths below are ignored local output, not committed artifacts. Temporary
probe scripts were deleted after investigation. No statistical performance
comparison was rerun or inferred from these correctness checks.

- `benchmarks/results/setup-probe-20261006-143830/`: 48 serial pre-fix warmup
  cases, 4096 clients each, Weave sharded/shared and Asio. All reached readiness.
- `benchmarks/results/setup-probe-20261006-144103/`: 64 serial post-fix warmup
  cases, 4096 clients each, including Tokio. Every client completed eight
  byte-validated warmup roundtrips.
- `benchmarks/results/setup-probe-20261006-144540/`: 32 post-fix validated cases,
  eight per backend, up to four servers concurrently, 1024 clients per server.
  Four workers per server; eight workers per client process. Includes eight
  warmup roundtrips, a short validation burst, and the normal cleanup handshake.
  Timing results were discarded.
- `benchmarks/results/asan-pressure-20261006-144724/`: three concurrent sanitizer
  instances, repeated for three rounds. Each instance ran the existing 1024-client
  warmup/disconnect test in both IOCP layouts and both completion modes. All nine
  instances passed, without relaxing the ten-second task timeout or finding a
  sanitizer error.

Larger exploratory parallel probes in `setup-probe-20261006-144228/`,
`setup-probe-20261006-144327/` and `setup-probe-20261006-144458/` failed their
five-second client-process cleanup waits after readiness, not the setup barrier.
They are retained as incomplete evidence, not counted as successful complete
matrices. The first two forcibly terminated warm clients; the third used the
normal handshake. The follow-up below isolates that cleanup failure.

## Cleanup Follow-Up

Instrumenting the common external Asio load generator separated native socket
closure from worker shutdown. Under four simultaneous 4096-client runs,
serial closes made steady progress through every socket but took 5-12 seconds.
The Asio pool's thread joins then completed promptly. This was not a stuck
Weave task: the same load generator exhibited it against every server backend.

Parallel closes and abortive linger did not reliably keep teardown below five
seconds. Those experiments were reverted; library socket semantics were not
changed. The harness had incorrectly assigned one five-second process-exit wait
to bulk closure plus process shutdown under artificial host-wide pressure.

The client now acknowledges `CLOSED` after all checked socket closes. The
harness allows that bounded phase 25 seconds, then applies the original
five-second limit to process exit. The total job deadline remains 300 seconds;
readiness and measurement limits are unchanged. Errors record separate
`client socket cleanup` and `client process exit` phases. This fixes attribution
and the reproduced cleanup timeout; it does not make socket closure faster.

- `cleanup-probe-20261006-150103/`: pre-fix diagnostics showed continued close
  progress, including 11.7-second cleanup, followed by prompt worker joins.
- `cleanup-probe-20261006-150321/` and `cleanup-probe-20261006-150458/`:
  unsuccessful parallel/abortive-close experiments; retained as incomplete.
- `cleanup-probe-20261006-150727/`: all four post-fix complete cases passed.
- `cleanup-probe-20261006-150829/`: all 12 post-fix complete cases passed, three
  per backend with four servers running concurrently. Bulk close times ranged
  from 4.28 to 10.95 seconds, including cases beyond the old combined limit;
  process exit passed its unchanged five-second boundary after acknowledgment.

These are byte-validation and cleanup probes, not statistical performance
results. Some earlier exploratory runs also reported setup EOFs with empty
native logs; those do not identify the historical warmup failure and are not
counted as complete successful runs.

Backlog-fix validation: Release CTest passed 16/16. Debug passed 15/16 initially; its
packaging consumer compile hit the existing 60-second command timeout, then the
isolated packaging rerun passed without source or deadline changes. ASan passed
15/15 with packaging excluded, plus the nine-instance concurrent stress above.
Rust's existing test and C++/Rust formatting checks passed. Full-suite logs and
the initial packaging failure are retained in the sanitizer evidence directory.

Neither historical failure was reproduced with identifying diagnostics. The
backlog defect is proven and fixed, but the original benchmark setup failure
and sanitizer timeout remain unattributed. Passing repetitions do not close them.
