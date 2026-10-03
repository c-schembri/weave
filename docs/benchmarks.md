# Benchmark methodology

The initial suite compares Weave and standalone Asio 1.36.0 with C++ coroutines
and exception-free operational error handling. Google Benchmark 1.9.4 drives
both. Dependencies are pinned by commit in CMakeLists.txt.

Each case uses one persistent TCP connection to the SAME blocking echo-peer
implementation on a separate thread. Both clients enable TCP_NODELAY, use
preallocated payload buffers, write all bytes, and read exactly the same number.
Setup, connection establishment, and teardown are outside the timed state loop.
Both runtimes are driven once for the entire loop, not restarted per message.
Asio uses Windows IOCP; Weave uses GetQueuedCompletionStatusEx.

Payload sizes: 64 B, 1 KiB, and 64 KiB. One operation pair is outstanding per
case. Elapsed time is per roundtrip; bytes/sec counts BOTH directions. This is
serial loopback request/response latency, not maximum network throughput, a
many-client scalability test, or an HTTP benchmark. Peer scheduling and the
Windows TCP stack may dominate small implementation differences.

The payload is checked after each benchmark case. Peer failures and operation
errors invalidate the case. Tests perform additional content validation across
fragmented transfers and larger payloads. No timed buffer allocation is required
by the harness, but library coroutine frames may allocate during measurement.

Run scripts/bench.ps1 on a quiet machine, preferably with a stable power plan.
The script uses randomized case interleaving, five repetitions, and 0.5 seconds
minimum per repetition. It records machine details, dirty worktree status, Git
revision, and raw Google Benchmark JSON. Keep raw data when reporting results.

Do not infer application tail latency from repetition statistics. Do not compare
Debug against Release, different payloads, or different connection concurrency.
Windows thread CPU-time resolution can make short-case CPU counters noisy or
zero; use real_time for this suite. Repeat close results before drawing conclusions.

The initial exploratory run showed comparable performance, not a demonstrated
Weave advantage. The first reproducible baseline belongs under benchmarks/results
with its raw data and environment, not only a favorable summary table.
