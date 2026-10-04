# Multicore runtime

`#include <weave/runtime.hpp>` and `weave::runtime` add an explicit worker runtime on top of Context.
TCP applications also include `<weave/tcp.hpp>` and link `weave::tcp`;
neither module depends on the other.
Each worker owns one thread and one IOCP. Choose a scheduler at construction;
there is no CPU-core pinning and no runtime mode switching.

```cpp
weave::Runtime runtime({
  .workers = 4,
  .scheduler = weave::Scheduler::work_stealing,
});
```

| Mode | spawn | spawn_on(index, factory) |
| --- | --- | --- |
| Scheduler::worker_affine (default) | Round-robin assignment; all resumptions stay on that worker | Always stays on the chosen worker |
| Scheduler::work_stealing | Runnable tasks can move between workers | Always stays on the chosen worker; never stolen |

In stealing mode, external submissions are distributed round-robin and nested
spawn calls start in the current worker's queue. Idle workers steal ready work.
Yield, I/O, and join continuations are eligible, not just tasks that have never
started. A task's factory receives the Context of its initial execution worker;
that reference remains usable after migration within the same runtime. The task
and its directly awaited/when_all children execute serially, even as they migrate.
Use spawn to request parallel execution of independent tasks.

This is not Tokio feature parity: there is no preemption, blocking-work pool,
timer service, or individual task abort. A running task is never stolen; it must
suspend or finish. Use `co_await ctx.yield()` for cooperative scheduling. Worker
IOCP drivers remain sharded, so blocking a worker also delays completion dequeue
for its port. Stealing does not make synchronous blocking harmless.

## Usage

```cpp
#include <weave/runtime.hpp>

int main()
{
  weave::Runtime runtime({.workers = 4});
  if (!runtime.status())
    return 1;

  auto job = runtime.spawn([](weave::Context &ctx) -> weave::Task<int> {
    co_await ctx.yield();
    co_return 42;
  });
  if (!job)
    return 1;

  auto result = std::move(*job).get(); // Result<int>
  runtime.join();
  return result == 42 ? 0 : 1;
}
```

`workers = 0` uses `std::thread::hardware_concurrency()`, including logical
processors, with a one-worker fallback. Runtime construction starts the workers;
it is not necessary to call run() or drive the event loop from the main thread.
`status()` reports IOCP startup errors; TCP setup reports Winsock errors.
Allocation or OS-thread resource
exhaustion is fatal under the current exception-free policy.

The factory executes on its selected (or stealing) worker and receives its Context.
The runtime owns the factory until its returned coroutine finishes. In particular,
captured coroutine lambdas do not dangle when spawn returns. For accepted tasks,
factory captures are destroyed on a runtime worker before the join result becomes
ready: the assigned worker in affine/pinned tasks, or the finishing worker for a
movable task.
A rejected factory is destroyed on the submitting thread. Create sockets in
the factory, not on the submitting thread, unless using an explicitly matching
worker-owned resource with spawn_on in affine mode.

## Results and joins

- `spawn(factory)` returns `Result<JoinHandle<T>>` for a factory returning Task<T>.
- Every Task is fallible. Socket and nested Task failures propagate automatically;
  `co_await as_result(operation)` yields `Result<T>` for explicit recovery.
- `std::move(handle).get()` waits on a non-runtime thread and consumes the handle.
  It returns `Result<T>`, including `Result<void>` for a void task.
  Blocking get(), join(), shutdown(), or runtime destruction inside a running
  Context is a fatal contract violation; use an asynchronous join instead.
- `co_await std::move(handle)` yields `T` or propagates failure, suspending the
  caller without blocking its worker. Use `as_result(std::move(handle))` to recover.
  Affine/pinned callers resume on their assigned worker; movable callers may
  resume on another worker. Standalone `Context::run` callers still resume on their
  own Context thread. A child's scheduling choice does not change its parent's.
- Handles are move-only and single-consumer. Dropping one does not cancel its
  task. The runtime retains ownership until completion, then releases an unclaimed
  result. Task itself forbids destroying a running or pending computation.

For example, either scheduler can spawn another task and await it with one worker:

```cpp
auto parent = runtime.spawn([&runtime](weave::Context &) -> weave::Task<int> {
  auto child = co_await runtime.spawn([](weave::Context &) -> weave::Task<int> { co_return 42; });
  co_return co_await std::move(child);
});
```

C++ has no Rust Send/lifetime checker. Borrowed captures must outlive the task.
Capture/result types must permit ownership transfer between their relevant
threads. In stealing mode, do not hold thread-owned locks across suspension or
assume thread_local values follow a task. Select worker_affine or spawn_on for
thread-bound resources. Weave's serialization covers its own awaiters; custom
awaiters that directly resume coroutine handles outside the scheduler are not
supported.

In affine mode, TcpStream/TcpListener remain thread-confined. In stealing mode,
their owning task may migrate, and ownership may be transferred between tasks
in the same runtime. A socket still belongs to its original Context/IOCP, not
the current execution worker. A live socket must not escape to the main thread
or another runtime, and its runtime must outlive it.

An individual socket/listener is not generally safe for concurrent access from
separately spawned tasks. Transfer ownership or serialize such access. Using
when_all for duplex read/write inside one task is supported: the kernel I/O runs
concurrently, while coroutine resumptions remain serialized. The library's
socket-registry lock protects backend lifetime/cancellation, not arbitrary user
objects or application data.

In stealing mode, an accept loop can move an accepted socket into a new spawned
handler. The handler may execute on any worker; the socket's IOCP registration
does not move. Simply accepting a connection does not spawn a handler for it.
In affine mode, accepted streams and handlers must stay on the listener's worker.

## Shutdown

`join()` closes submissions and drains all accepted tasks, then joins the workers.
Further spawn calls, including nested calls from existing tasks, return
operation_canceled. Await root tasks before calling join if they still need to
spawn children. join does not cancel I/O and may wait indefinitely for it.
Runtime-owned sockets/listeners must also be released before their Context exits.
Prefer task-owned resources. If retaining sockets in external application state,
close them on a valid runtime worker (their owner in affine mode) before closing
runtime submissions.

`request_stop()` is thread-safe and may be called by a worker. It closes submissions
and wakes every worker. Each worker requests CancelIoEx for its managed sockets,
rejects later I/O, and continues draining completions and joins. Cancellation can
lose to an already completed operation. Task code propagates the I/O error or
recovers through `as_result`, and/or checks ctx.stop_requested() after yielding;
Runtime::stop_requested() is
also safe to inspect from other threads.

`shutdown()` requests stop and joins. The destructor does the same. Repeated calls
are allowed. This is cooperative shutdown, not forced coroutine destruction:
unbounded CPU work, code that ignores stop and loops forever, or a join waiting on
uncooperative external work can prevent shutdown. There is no individual task
abort or stop token yet. Runtime must outlive concurrent calls to its methods.
Unexpected completion-port or cancellation-control failures remain fatal runtime
invariants; request_stop is not an error-recovery API for a broken backend.

## Implementation boundaries

Affine contexts retain lock-free socket-submission/registry access on their owner
thread. Stealing contexts allow submission from workers in the same runtime,
using a shared submission lock versus exclusive handle registration, close, and
shutdown cancellation. This prevents new submissions slipping past cancellation
and prevents cancellation racing closed/reused handles. Pending operations retain
their buffers and OVERLAPPED storage through completion, including cancellation.
This addresses the new-submission race described in Microsoft's
[I/O cancellation guidance](https://learn.microsoft.com/en-us/windows/win32/fileio/canceling-pending-i-o-operations).

Only runtime-managed contexts track live socket handles. Each port is still
polled only by its owner. Completion packets schedule continuations onto the
owning task's serialized ready queue in stealing mode; affinity mode resumes
directly. Context metrics use atomic updates in stealing mode, and metrics()
returns a snapshot by value. Concurrent fields in that snapshot need not describe
one instant in time. Scheduler wake packets are not socket-completion metrics.

Each spawned task has one shared result/control allocation with two intrusive
references (runtime and consumer), not a shared_ptr per socket operation. Ready
publication and waiter registration use one atomic handshake to avoid lost wakes
or duplicate resumes. A stealing dispatch temporarily retains its task while
resumption unwinds, so a result consumer cannot free an executing control block.
Task frames and captures are reclaimed on a valid runtime worker.
The result may be destroyed on a runtime worker or on the thread consuming or
dropping its handle, depending on which reference is released last.
The coroutine-frame cache remains thread-local; spawning is not allocation-free.

Global submission locking coordinates admission and shutdown. Stealing adds
mutex-protected per-worker queues and an intrusive per-task continuation queue;
there is no heap allocation per queued I/O continuation. Worker deques can
allocate blocks, and task control blocks/frame caches still allocate as needed.
Ordinary affinity completions do not take scheduler queue locks.

An idle worker publishes its idle state before rechecking ready queues; enqueuers
wake it through its own IOCP. This closes the enqueue/park lost-wakeup race.
Workers alternate pinned/movable local work and service I/O after at most 64
ready dispatches. Active roots and dispatches must both drain before contexts
are destroyed. This is a correctness-first stealing baseline, not a lock-free
scheduler or a strict fairness/tail-latency guarantee.
