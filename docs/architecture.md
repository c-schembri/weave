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
  io/         Context, completion engine, task ownership, spawn and JoinHandle
  runtime/    Worker threads, scheduling policies, cross-context coordination
  tcp/        Sockets, listeners, connect/accept, reads/writes, native socket lifetime
  local/      Local/Unix-domain streams, listeners, Linux peer credentials
  sync/       Bounded channels, semaphore permits, cancellation-aware wait queues
  tls/        Optional OpenSSL engine and generic encrypted-stream adapter
  postgres/   Native PostgreSQL protocol, authentication, queries and blocking facade
tests/
  integration/   Contracts spanning runtime, TCP, and tasks
  package/       Isolated builds and installed consumer/header checks
test_support/    Shared fixtures, never part of the library
benchmarks/
  support/       Shared comparison harness, runner, and Asio helpers
  integration/   Whole-system concurrent throughput/CPU/tail-latency workload
  results/       Local benchmark outputs, ignored by Git
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

`<weave/tcp/serve.hpp>` adds concurrent client dispatch using IO's existing
`TaskScope` and submission hooks. It retains handlers and drains clients before
returning, without depending on Runtime or adding another execution layer.

```text
core <- io <- {tcp, sync, runtime, local}
                ^     ^
                 \   /
                  tls -> OpenSSL 3.5+
                   ^
                postgres -> Local + OpenSSL Crypto + ICU (private dependencies)

TCP, Local, Sync and Runtime do not depend on each other.
TLS does not require Runtime; Core's stream contracts have no OS dependency.
PostgreSQL uses Local privately and does not require Runtime or libpq.
libpq is an optional benchmark dependency only.
```

`WEAVE_MODULES` selects build roots, with dependencies added automatically.
Each component has its own exported CMake target and install export. A consumer
can request `find_package(weave CONFIG REQUIRED COMPONENTS tcp)` without creating
or linking a runtime target. Only core is header-only; the other targets are
static libraries for now. Shared-library ABI/export support is not implemented.
No library-only configure fetches test or comparison dependencies.

The `package_components_<module>` CTests build each selected component alone,
install and relocate it, compile every installed header independently, and run an
external consumer linked only to that component. They reject unexpected targets,
dependencies, native headers in public headers, and missing required components.
The TCP consumer performs a loopback exchange without the runtime. Runtime
consumers exercise both schedulers without TCP.
The combined `package_components_all` case checks a multi-component install.
Each case has its own timeout rather than sharing one cold-build time budget.

### Namespace visibility

Public concepts and their member definitions live in `weave` (including the
`weave::tcp` entry points). Shared internal contracts live in `weave::detail`:
`IoAccess`, `Operation`, `Posted`, execution state, timer records/queues, and
submission/scheduler machinery. Header-required template helpers also belong there;
being private alone is not a reason to use `detail`.

Library `.cpp` files keep file/platform-specific helpers, types, and state in an
anonymous namespace inside `weave`, before public/member implementations. Windows
TCP's socket helpers and operation guard remain file-local. TCP and Local now
share native socket awaiters in IO's private `src/<platform>/socket.hpp`; these
retain the existing submission, cancellation and completion routing, with a
Windows address-buffer capacity parameter for the larger Unix-domain address.
Neither transport reaches into the other. Timer awaiters, IOCP completion keys,
and the runtime's current-task pointer are local to their respective implementations.

Private nested `Context::Impl` and `Runtime::Impl` definitions stay in their
enclosing namespace as C++ requires. `detail::schedule` stays named because its
header-declared friendship grants access to Runtime's private implementation.
Backend hooks such as `post`, `context_cancellation`, `IoAccess`, and `ContextAccess`
keep external linkage so separately compiled modules and header-instantiated
submission/join code can call their implementations. No additional runtime layer
or public API is introduced by these visibility boundaries.

### Future protocols

Add HTTP, WebSocket, and other modules as real features arrive,
not as empty placeholder directories. Each gets its own include entry point and
target with the smallest honest dependencies. Protocol code should depend on a
transport contract rather than worker scheduling policy or platform internals.
Do not add virtual stream hierarchies until actual protocol implementations
establish the requirements.

The current stream contracts are concepts in Core, not an inheritance hierarchy.
TLS composes an owned `CancellableStream` with a private compiled OpenSSL engine.
OpenSSL/native types stay out of public headers. The adapter serializes engine
steps and uses Sync semaphores to coordinate encrypted transport reads/writes.
It depends on public transport operations, not IOCP or Runtime internals.

Sync's intrusive waiter record lives in its suspended coroutine frame. A primitive
lock arbitrates delivery/grant, close and cancellation. Cancellation callbacks
are registered before publication and disarmed outside that lock before cleanup;
completion posts through the captured Context/executor. This shared machinery
belongs to Sync's detail headers, not TCP or the platform completion backend.

WebSocket framing/session code must not require a complete HTTP client/server
stack. Its standard opening handshake still has HTTP semantics; independence
does not remove that protocol requirement. Keep HTTP-stack integration in an
optional adapter target, and decide ownership of a proven handshake parser when
implementing it. PostgreSQL uses public TCP/TLS operations, not HTTP or private
completion-engine contracts. Its installed API contains no native, OpenSSL, ICU,
or libpq types. Private wire/authentication contracts shared by implementation
files stay under `postgres/src/`; they are not installed.

PostgreSQL owns a stable connection implementation and serializes protocol
operations. SQL failures drain through ReadyForQuery before allowing reuse;
incomplete transport/protocol operations close the session rather than guessing
where the wire stream resumes. Independent cancellation snapshots own their
credentials and server cancellation key. BlockingConnection drives the same
engine on an owned caller-thread Context, without a helper thread. Feature and
qualification status lives in the [PostgreSQL parity checklist](postgres-parity.md).

## Data and execution

The Windows backend described below uses a private allocation and contiguous 64-entry
completion buffer. Standalone Contexts and sharded runtime workers own their
IOCP; shared runtime workers borrow one runtime I/O domain's port. The public
Context has no native OS types. Linux uses the same public ownership/execution
model with an io_uring backend; see the Linux lifetime section below.
`Context::create(options)` returns `Result<Context>`: IOCP setup errors are returned
before any Context is published. Guaranteed copy elision constructs the immovable
Context directly in its owning result, with no extra allocation. A private factory
key permits in-place construction without exposing an unchecked constructor.
Sockets and queued continuations borrow the Context's stable address, so the result
must outlive them. The runtime creates and owns each worker's result on that worker.

`Runtime::create(options)` likewise returns an immovable `Result<Runtime>` only
after worker startup succeeds. Workers borrow the stable backend allocation during
startup, before the Runtime object is constructed in-place. Startup failure stops
and joins started workers before returning the error. `Runtime::run` submits a root
and waits on the calling thread; workers drive the event loops. It does not close
admission or join independent work. Runtime destruction cancels and drains that work.

Context is an independent execution unit, not a thread. It accepts thread-safe
task or factory submissions and drives them on its owning/calling thread. Factories
may take no arguments or Context &; direct tasks transfer an already-created frame.
Spawned factories,
root tasks, deferred cleanup, and JoinHandle publication share one implementation
in IO; Runtime adds scheduling state rather than a second task-lifetime model.
Standalone context tasks stay on their owner. A custom coordinator can create
contexts on its own threads, submit through Context::spawn, drive Context::run(),
and request stop without including or linking Runtime. See [Context](context.md).

All contexts register their sockets, including sockets created before run(), so
cooperative stop can cancel and drain pending I/O. Admission and stop share a
submission lock; accepted roots retain ownership until deferred cleanup completes.
Runtime accounts for Context::spawn tasks submitted on its workers as well as its
own scheduled roots, and closes both admission paths before declaring itself drained.

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

I/O layout is independent of scheduling: the sharded control remains the default;
the opt-in shared IOCP is serviced by every runtime worker. Completion collection
does not transfer execution ownership. Movable roots use their serialized executor;
affine/Context-owned work is routed to the owner's intrusive ready queue. Targeted
no-op APCs interrupt alertable waits without running user code. The port has one
shared lifetime allocation per runtime domain, not per operation. Shutdown retains
all worker Contexts until both scheduled dispatches and dequeued completion batches
have unwound, including cross-worker publication after a task finishes.

## Linux lifetime rules

Linux Contexts own one liburing ring and one eventfd wake read. The owner alone
accesses SQ/CQ; migrated task submissions/cancellations arrive as intrusive
Context messages. Completion posts through the captured executor, not whichever
thread requested cancellation. No Weave I/O or timer helper threads are added.

The suspended operation embeds native submission, cancellation and continuation
records. Cancellation records whether a command is queued or awaiting its own
CQE. Completion does not publish until the original request and any cancellation
acknowledgement have drained. A successful accepted fd is either attached to the
original Context or closed if stop/setup wins. Closing an fd is not cancellation.

CQ backpressure temporarily buffers completions without running callbacks under
native-operation locks. The normal polling path consumes them with the same
bounded dispatch and lifetime rules. The permanent wake read is also explicitly
cancelled and drained before ring/storage destruction.

Linux shares the steady-clock timer policy, task ownership, schedulers, Sync and
TLS adapters. DNS uses glibc notification callbacks retained through cancellation,
including lookups that cannot be interrupted. Shared I/O layout is rejected,
not reinterpreted as sharded. [Requirements and build](linux.md).

## Task completion

`Task<T>` is the only public coroutine type. Its promise stores explicit running,
succeeded, and failed states, an owned-child link, a failure-parent link, and a
success continuation. A failed child routes control to the nearest result
boundary without resuming the skipped bodies. Those parents are still suspended
at their awaits, not at final_suspend; handle.done() is not a completion test.

Context drives a native Task root. Context::spawn and Runtime::spawn retain each
native root and any factory, then defer root cleanup until resumption unwinds before
publishing the join result on a valid execution thread.
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

Contextless TCP setup is lazy and uses IO's thread-local active Context when the
setup Task begins execution. Context drivers establish that scope; no Runtime
dependency or implicit event loop is introduced. No active Context is a fatal
contract violation. Explicit Context overloads remain available, including
synchronous listener setup. Socket/listener objects retain the selected Context
and IOCP registration across coroutine migration; subsequent operations never
rebind them to the current worker. Endpoint strings are borrowed through setup.

Free detach resolves an IO-owned submission scope, distinct from the current
root's continuation Executor. Standalone/custom Context drivers and affine
workers submit locally. Stealing workers install a coordinator hook and use
shared scheduler-compatible root storage to submit independent movable roots,
with the same single ownership allocation as explicit Runtime detach. Runtime
startup publishes its owning object before tasks can use that hook; Context
leave clears the thread-local submission scope. The scope selects submission,
not resource binding or parent-child joining, and introduces no IO-to-Runtime
link dependency. Explicit member submissions remain available outside execution.

TCP also validates execution compatibility before asynchronous entry/submission.
An owner-thread match alone is insufficient: a foreign Context must be in the
same stealing domain and have a runtime continuation Executor, or fail a contract.
This prevents an unrelated same-thread loop from awaiting completions queued to
another port, and prevents Context-owned tasks from accidentally migrating.
Synchronous explicit listener setup and cleanup do not require an active loop.

TCP shares IO's private `src/windows/iocp.hpp` or `src/linux/uring.hpp` backend contract only at build
time. This dependency does not enter installed headers or exported include paths.
Runtime uses a platform-neutral ContextAccess contract. IO routes stolen
continuations through a non-owning Executor hook, not a hard reference to a
runtime function; standalone continuations still resume directly. Shared-port
affine continuations collected elsewhere are queued to their owning Context.
The runtime retains its existing root serialization and lifetime rules.

The scheduler hook adds no per-operation allocation or virtual object hierarchy.
This split is structural, not a measured performance improvement; no performance
parity claim follows from correctness tests. Both native backends preserve these
operation/completion semantics and are selected at build time.

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
