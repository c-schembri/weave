# Linux And WSL

Linux uses liburing's io_uring completion API. TCP, contexts, both runtime
schedulers, cancellation, timers, synchronization and optional TLS use the
same public API as Windows. Public headers contain no Linux or liburing types.

## Build

Requires GCC 14+ with C++23 `std::expected`, CMake 3.25+, liburing 2.3+, and
glibc. The kernel must provide `IORING_FEAT_EXT_ARG` and `IORING_FEAT_NODROP`
(Linux 5.11+), with io_uring enabled by the host's security policy. Context
creation returns the native setup error if the host denies ring creation.
There is no fallback to epoll.

Ubuntu/WSL, with a sufficiently recent compiler:

```sh
sudo apt-get update
sudo apt-get install g++ cmake ninja-build liburing-dev libssl-dev pkg-config git python3

cmake --preset linux-debug
cmake --build --preset linux-debug --parallel 4
ctest --preset linux-debug
./build/linux-debug/Debug/echo_server_context_minimal
```

Use `linux-release` or `linux-asan` for the other validation configurations.
Development presets include optional TLS; library-only builds keep TLS optional:

```sh
cmake -S . -B build/tcp -G Ninja -DCMAKE_BUILD_TYPE=Release -DWEAVE_MODULES=tcp
cmake --build build/tcp --parallel 4
cmake --install build/tcp --prefix "$HOME/.local"
```

Installed IO imports discover liburing through pkg-config. Core-only builds
and consumers do not discover liburing, OpenSSL, or Python. TLS alone discovers
OpenSSL; Linux trusts its system/default certificate paths.

WSL runs the real Linux backend. Prefer a Linux-filesystem build directory for
faster builds when source lives under `/mnt/c`; configure with `-B` and use
`cmake --build <directory>` / `ctest --test-dir <directory> --output-on-failure`.

## Execution And Lifetime

Each Context owns one ring, its completion consumer, and an eventfd wake read.
Submissions and cancellations from migrated tasks are queued to that Context's
owner. Only the owner modifies liburing's SQ/CQ; user continuations return
through their captured executor, preserving affine tasks and root serialization.
No Weave I/O driver thread, timer thread, SQ polling thread, or busy-poll loop
is introduced. glibc may use its own resolver and notification threads for DNS.

Software work is queued intrusively. Eventfd wakes a parked owner without
running user code on the caller. Timers use steady-clock deadlines and a wait
bounded by the nearest deadline, not a periodic polling timeout.

Native operations and buffers remain alive through completion. Cancellation is
another io_uring request: Weave waits for both the original completion and any
cancel acknowledgement before posting the suspended coroutine. Successful I/O
can win a cancellation race. An accepted descriptor that cannot be registered
after a concurrent stop is closed before propagating the error.

Closing a socket does not cancel a borrowed native operation. `close()` rejects
pending reads/writes/accepts; cancellation must drain first. Context destruction
also cancels and drains its own wake read before freeing its native storage.
Completion-queue backpressure retains completions without resuming user code
under an operation lock, then dispatches them through the normal path.

## Platform Differences

- `IoLayout::sharded` is supported with worker-affine and work-stealing schedulers.
  `IoLayout::shared` returns `operation_not_supported`; it does not silently
  create sharded rings. A shared Linux collector needs its own ownership/wakeup design.
- `skip_successful_completions` is a Windows IOCP tuning option. Linux accepts
  the option but always drains a native CQE; there is no fabricated inline-success path.
- Linux listeners use `SO_REUSEADDR` for restarts after TIME_WAIT, not `SO_REUSEPORT`.
  Competing listeners on the same endpoint still fail. IPv6 defaults to v6-only.
- Socket failures use the standard generic errno category. DNS errors use
  `weave.resolve`; cancellation reports `operation_canceled`.
- Hostname lookup uses glibc [`getaddrinfo_a`](https://man7.org/linux/man-pages/man3/getaddrinfo_a.3.html),
  preserving system resolver policy.
  Already-running lookups may not be interruptible: cancellation/timeout still
  drains the notification. A glibc notification-record allocation failure is
  fatal, consistent with Weave's allocation policy, rather than freeing a live query.
- Runtime trace recording and the native performance benchmark harness remain
  Windows-only. No Linux performance comparison has been established yet.

Correctness tests cover both schedulers, native cancellation/EOF/partial I/O,
IPv4/IPv6/DNS, tiny-ring completion pressure, TLS interoperability and relocated
component consumers. Shared-layout collector tests remain Windows-only.

Local validation on WSL2 Ubuntu 26.04, GCC 15.2, liburing 2.14 and OpenSSL 3.5.5:
all 29 CTest entries passed in Debug, Release and AddressSanitizer configurations.
This is local correctness evidence, not a Linux CI or performance result.
