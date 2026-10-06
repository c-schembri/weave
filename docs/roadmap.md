# Development priorities

Preserve the small, exception-free API, explicit lifetime model and optional
module boundaries. Implement these stages in order rather than building all
features at once. Performance measurements are manual and only needed for
intentional performance work; correctness CI never runs benchmarks.

## 1. Correctness hardening

The first pass adds a single-listener, 1024-client warmup/disconnect regression
under both IOCP layouts and both successful-completion modes. Debug, Release and
AddressSanitizer are the validation configurations. Benchmark failure diagnostics
must identify setup versus measured-I/O failures and preserve incomplete evidence.

Still open: the 2026-10-06 sharded/shared diagnostic's setup/warmup failure. Its
original logs are insufficient to identify the cause. Passing later runs or new
regression tests does not establish a fix. Use the new diagnostics to narrow a
recurrence, then add a targeted reproducer before changing runtime behavior.

The new 1024-client test also timed out once in shared-IOCP, queued-success mode
with 948 clients warmed while Debug, Release and sanitizer suites were running
concurrently on the same host. The sanitizer integration suite subsequently
passed in isolation and in three consecutive repetitions, without increasing
the test's deadline. This is not proof that the two failures share a cause or
that connection setup under host-wide pressure is reliable.

Further hardening should cover admission/shutdown races, cancellation and
completion races, connection resets, partial I/O, and buffer/frame destruction.
Do not change scheduling defaults based on a single benchmark host.

## 2. DNS and IPv6

Introduce a useful address/endpoint representation and IPv6-capable transports,
then asynchronous hostname resolution and multi-endpoint connection handling.
Numeric-address operations must remain available without performing DNS. Resolver
buffers and cancellation must drain before their owning tasks can finish. Keep
the common connect/listen path simple and do not expose Windows types publicly.

## 3. Channels and semaphores

Start with bounded asynchronous channels and a cancellation-aware semaphore.
Specify closure, waiter ownership, fairness, backpressure and cross-thread wakeups
before implementation. Suspended waiters must be removed or drained before their
coroutine frames can be reclaimed. Do not block I/O workers while waiting.

## 4. Composable streams and TLS

Use a small stream contract so protocol helpers can operate over TCP, TLS and test
streams without requiring an inheritance hierarchy. Keep TLS optional, use a proven
TLS implementation, and specify certificate verification and shutdown behavior.
Do not implement cryptography or force TLS dependencies onto TCP-only consumers.

## 5. Linux io_uring

Implement the same task, completion, cancellation and lifetime contracts with
io_uring. Add Linux correctness CI and platform-matched measurements before making
cross-platform performance claims. No epoll fallback or macOS backend is planned.
