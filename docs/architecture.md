# Architecture

## Priorities

Correctness is non-negotiable. Simple API ergonomics are the primary product goal:
keep common workflows obvious without hiding ownership costs. Measure throughput,
latency, memory usage, and CPU efficiency when making performance decisions;
do not invent performance claims or add complexity without evidence.

## Module boundaries

```text
modules/
  core/       Task, Result, numeric types, coroutine frame allocator
  io/         Context, completion engine, handle registry, continuation routing
  runtime/    Worker threads, scheduling policies, spawn and join
  tcp/        Sockets, listeners, connect/accept, reads/writes, Winsock lifetime
tests/
  integration/   Contracts spanning runtime, TCP, and tasks
  package/       Isolated builds and installed consumer/header checks
test_support/    Shared fixtures, never part of the library
benchmarks/
  support/       Shared comparison harness, runner, and Asio helpers
  integration/   Whole-system concurrent throughput/CPU/tail-latency workload
  results/       Historical evidence, kept at its original paths
cmake/           Library build/install helpers
```

Each module owns `CMakeLists.txt`, `include/weave/`, `src/` when compiled,
and its `tests/`, `examples/`, and `benchmarks/` where applicable.
Ownership follows the primary feature: an echo example belongs to TCP even
when it uses Runtime. Competing-library implementations live next to the feature
they compare, not in a separate library-specific tree.

Core owns coroutine comparisons, runtime owns CPU/scheduler comparisons, and
TCP owns echo/transfer/connection comparisons, including multicore TCP.
`benchmarks/CMakeLists.txt` sets up shared dependencies and assembles each
module's benchmark object library into `weave_bench`. Object libraries retain
all static benchmark registrations without whole-archive linker flags.
Example targets are declared under their module's examples directory.
Their dependencies never enter exported library targets.

The current coroutine benchmark driver still uses Context, so comparison
builds require the IO module even for core-only measurements. This is a harness
dependency, not a change to core's library dependency graph. Runtime and TCP
benchmark groups are included only when their modules are enabled; multicore
Weave TCP cases and the whole-system gate require both. No workload logic was
changed to make the directory reorganization fit.

The public include spelling does not expose the repository layout:
`<weave/tcp.hpp>` includes that feature, while `<weave/tcp/stream.hpp>` is a
narrower entry point. Installed template internals live under the owning
feature's `detail/`; private compiled implementation headers stay in `src/`.
There is no global library `include/`, `src/`, or catch-all `detail/` directory.

```text
weave::core
    ^
weave::io
    ^             ^
weave::tcp    weave::runtime
   (both depend on io, not on each other)
```

`WEAVE_MODULES` selects build roots, with dependencies added automatically.
Each component has its own exported CMake target and install export. A consumer
can request `find_package(weave CONFIG REQUIRED COMPONENTS tcp)` without creating
or linking a runtime target. Only core is header-only; the other targets are
static libraries for now. Shared-library ABI/export support is not implemented.
No library-only configure fetches test or comparison dependencies.

The `package_components` CTest builds each selected component alone, installs
and relocates it, compiles every installed header independently, and runs an
external consumer linked only to that component. It rejects unexpected targets,
dependencies, native headers in public headers, and missing required components.
The TCP consumer performs a loopback exchange without the runtime. Runtime
consumers exercise both schedulers without TCP.

### Future protocols

Add HTTP, WebSocket, PostgreSQL, TLS, and other modules as real features arrive,
not as empty placeholder directories. Each gets its own include entry point and
target with the smallest honest dependencies. Protocol code should depend on a
transport contract rather than worker scheduling policy or platform internals.
Do not add virtual stream hierarchies until actual protocol implementations
establish the requirements.

WebSocket framing/session code must not require a complete HTTP client/server
stack. Its standard opening handshake still has HTTP semantics; independence
does not remove that protocol requirement. Keep HTTP-stack integration in an
optional adapter target, and decide ownership of a proven handshake parser when
implementing it. PostgreSQL should likewise depend on its transport and optional
TLS, not HTTP. We are establishing those boundaries, not implementing them now.

## Data and execution

Each Context owns a private backend allocation with one IOCP and a contiguous
64-entry completion buffer. The public Context has no native OS types.
GetQueuedCompletionStatusEx dequeues a batch. A completion points directly to
the Operation embedded in its suspended coroutine frame; there is no virtual
dispatch, per-operation shared_ptr, or separately allocated callback record.

Coroutine frames use a bounded thread-local cache with 128/256/512/1024-byte size
classes, up to 64 dead frames per class (120 KiB of retained payload per thread).
Allocation misses, larger frames, and over-aligned promises use the heap.
This is NOT a zero-allocation runtime. The initial operation awaiter still contains accept-specific scratch space
even on the receive/send paths; specialization is a candidate, not a measured win.

Only destroyed frames are recycled; active and suspended frames are never reused.
Frames have no pointer to a source cache, so destruction on a different thread
does not touch the allocating thread's storage. Cache contents are released at
thread exit. Later TLS destructors fall back to the heap, retaining size-class
rounding so frames can still be transferred to another live thread. This does
not itself change the runtime's Context/socket ownership rules. Out-of-memory terminates.

ASan builds poison cached dead frames and unused size-class padding, then
unpoison on reuse. `WEAVE_RECYCLE_FRAMES=OFF` disables coroutine caching for
controlled measurements; all translation units must use the same setting.

Operations update Context counters for submissions, completions, and dequeue
calls. Affine contexts use ordinary single-thread-owned counters; stealing
contexts use atomic_ref updates because submission and dequeue can occur on
different workers. The OS owns the completion queue. Do not add container or object hierarchies
without a demonstrated data-access need.

## IOCP lifetime rules

Sockets using an IFS provider opt into FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
when SetFileCompletionNotificationModes succeeds. The mode is tracked per
handle. Immediate successes then resume inline, using the returned byte count;
pending operations still await their kernel completion. Unsupported sockets or
ContextOptions{.skip_successful_completions = false} keep the original rule:
even immediate success MUST await its queued packet.

After 32 inline successes, post a synthetic completion and suspend to prevent
unbounded inline chaining. Synthetic completions carry the same operation record
and byte count. They are drained before frame destruction just like kernel
completions. Cancellation can lose to an already-successful operation.

Receive and send submissions are capped at 64 KiB. read_exactly loops directly
over the I/O awaiter rather than allocating a nested read coroutine per partial
receive. These policies are measured together: smaller sends without completion
suppression increased queue traffic and regressed the first experiment.

The private standard-layout Operation starts with OVERLAPPED, checked statically.
It embeds a separate platform-neutral Posted continuation record. Software-only
posted messages use a distinct completion key and carry an opaque Posted pointer;
they are not kernel-owned overlapped operations.
For queued operations, the operation and buffer remain alive until the packet
is dequeued. An inline success is already complete on return from Winsock. On
failed entries, the TCP awaiter's await_resume uses WSAGetOverlappedResult to
translate native status before releasing its pending-operation guard. The socket
and OVERLAPPED remain alive through that translation.

CancelIoEx requests cancellation, but does not join it. Closing or destroying a
borrowed stream before completion is forbidden. close() returns an in-progress
error rather than releasing memory still in use by the kernel.

The Context cannot be recursively pumped. Children return by symmetric transfer;
when_all retains their frames until the last child completes. Context alone does
not create threads. The optional [Runtime](runtime.md) coordinates independent
worker contexts with explicit task ownership and cross-thread spawn/join messages.
Its default worker-affine scheduler preserves thread confinement. An opt-in
work-stealing scheduler moves runnable task continuations, but not socket IOCP
registrations. It serializes each root task's resumptions, synchronizes socket
registry/submission access, and drains executing dispatches before context exit.
There is no implicit global runtime; see runtime.md for ownership restrictions.

## Task completion

`Task<T>` is the only public coroutine type. Its promise stores explicit running,
succeeded, and failed states, an owned-child link, a failure-parent link, and a
success continuation. A failed child routes control to the nearest result
boundary without resuming the skipped bodies. Those parents are still suspended
at their awaits, not at final_suspend; handle.done() is not a completion test.

Context drives a native Task root. Runtime owns each factory and its native root,
then schedules root cleanup on a valid worker before publishing the join result.
No alternative coroutine model or per-operation adapter sits around IOCP tasks.
Completed failed chains are destroyed iteratively, inside-out, before a result
observer resumes. Pending operations must still complete before their frames can
be reclaimed. `when_all` drains every child before propagating any error, keeping
parent-owned buffers alive for siblings. See [Task contracts](tasks.md).

## Platform boundary

Windows headers are confined to private implementations and native test helpers.
IO owns generic handle registration, completion dequeue, and cancellation.
TCP owns Winsock initialization, socket creation, submission, and error
translation. Each native socket holds a Winsock startup reference until successful
close, including setup failure, moves and migration. There is no process-global
destructor that can tear Winsock down before a late runtime shutdown.

TCP shares IO's private `src/windows/iocp.hpp` backend contract only at build
time. This dependency does not enter installed headers or exported include paths.
Runtime uses a platform-neutral ContextAccess contract. IO routes stolen
continuations through a non-owning Executor hook, not a hard reference to a
runtime function; affine and standalone continuations still resume directly.
The runtime retains its existing root serialization and lifetime rules.

The scheduler hook adds no per-operation allocation or virtual object hierarchy.
This split is structural, not a measured performance improvement; no performance
parity claim follows from correctness tests. When adding io_uring, keep these
operation/completion semantics and select the concrete backend at build time.

## Next evidence to collect

1. Cache hit/miss counts, retained memory, and coroutine frame sizes under churn.
2. Many-connection and pipelined throughput, not only serial loopback latency.
3. Completion batch occupancy and scheduler overhead under concurrent load.
4. Tail latency with backpressure and slow readers.
5. Cancellation/close races, long-running stress, and sanitizer runs.
6. Only then compare changes such as cache tuning, specialized operation
   storage, and larger batches.

## Platform references

- [GetQueuedCompletionStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatusex)
- [AcceptEx](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nf-mswsock-acceptex)
- [ConnectEx](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nc-mswsock-lpfn_connectex)
- [CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)
- [SetFileCompletionNotificationModes](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setfilecompletionnotificationmodes)
