# Multicore runtime

`#include <weave/runtime.hpp>` and `weave::runtime` add an explicit worker runtime on top of Context.
TCP applications also include `<weave/tcp.hpp>` and link `weave::tcp`;
neither module depends on the other.
Context already supports independent tasks and spawn/join on its calling thread.
Runtime adds worker management and scheduling; external coordinators can instead
use the [public Context contract](context.md) without linking this module.
Each worker owns one thread and one Context. Choose a scheduler and I/O layout at construction;
there is no CPU-core pinning and no runtime mode switching.

```cpp
auto runtime = weave::Runtime::create({
  .workers = 4,
  .scheduler = weave::Scheduler::work_stealing,
  .io_layout = weave::IoLayout::shared,
});
```

| Mode | spawn | spawn_on(index, task_or_factory) |
| --- | --- | --- |
| Scheduler::worker_affine (default) | Round-robin assignment; all resumptions stay on that worker | Always stays on the chosen worker |
| Scheduler::work_stealing | Runnable tasks can move between workers | Always stays on the chosen worker; never stolen |

`detach` uses the same scheduling policy as `spawn`; `detach_on` pins work just
like `spawn_on`. Neither returns a join handle. All four methods are thread-safe.

In stealing mode, external submissions are distributed round-robin and nested
spawn calls start in the current worker's queue. Idle workers steal ready work.
Yield, I/O, and join continuations are eligible, not just tasks that have never
started. A context-taking factory receives the Context of its initial execution worker;
that reference remains usable after migration within the same runtime. The task
and its directly awaited/when_all children execute serially, even as they migrate.
Use spawn or detach to request parallel execution of independent tasks.

TCP setup need not receive the factory's Context: `co_await tcp::listen(ipv4, port)`
and `co_await tcp::connect(ipv4, port)` resolve the current execution worker's
Context when their lazy tasks start. Constructing them on the caller is safe;
for example, `runtime->run(serve(port))` needs no Context-taking factory.
After migration, new setup uses the worker executing it, while existing sockets
retain their original Context/IOCP binding. The address string must survive setup.
Explicit Context overloads still select a particular binding; synchronous
`tcp::listen(ctx, ...)` returns `Result<TcpListener>`.

This is not Tokio feature parity: there is no preemption, blocking-work pool,
or forced task abort. Cancellation is cooperative; timers are provided by IO.
A running task is never stolen; it must suspend or finish.
Use `co_await ctx.yield()` for cooperative scheduling.
With the default sharded layout, blocking a worker also delays completion dequeue
for its port. The shared layout lets other workers collect that socket's completions;
movable tasks can then resume elsewhere, while pinned/Context-owned continuations
still wait for their owner. Neither layout makes synchronous blocking harmless.

## I/O layout

`IoLayout::sharded` (default) gives each worker a private IOCP with concurrency one.
`IoLayout::shared` gives the runtime one IOCP with concurrency equal to its worker
count, serviced by all workers. Scheduling policy and I/O layout are independent:
both affine and stealing schedulers support both layouts. `io_layout()` reports
the selected layout. Invalid layouts return `std::errc::invalid_argument`.

The shared layout is an experimental candidate, not a performance claim or a
default change. A socket still retains its originating Context for lifetime,
registration, cancellation, and affinity checks; sharing the native port does
not grant permission to access it from unrelated tasks or runtimes. Standalone
Contexts continue to own private ports and create no threads.

## Usage

```cpp
#include <weave/runtime.hpp>
#include <weave/log.hpp>

int main()
{
  auto runtime = weave::Runtime::create({.workers = 4});
  if (!runtime)
    return weave::report_error(runtime.error());

  auto result = runtime->run([](weave::Context &ctx) -> weave::Task<int> {
    co_await ctx.yield();
    co_return 42;
  });
  if (!result)
    return weave::report_error(result.error());
  return result == 42 ? 0 : 1;
}
```

`workers = 0` uses `std::thread::hardware_concurrency()`, including logical
processors, with a one-worker fallback. `create(options)` starts the workers and
waits for each worker's Context startup. It returns `Result<Runtime>` containing
either a usable runtime or the startup error. Invalid schedulers return
`std::errc::invalid_argument`; worker IOCP errors are returned only after stopping
and joining already-started workers. No failed runtime is published and there is
no default constructor or separate `status()` check. TCP setup reports Winsock
errors. Allocation or OS-thread resource exhaustion remains fatal under the
current exception-free policy.

The runtime is constructed directly inside its result. Both are immovable because
tasks borrow the Runtime's address. Use `runtime->member()` and pass `*runtime` to
functions taking `Runtime &`; keep the result alive through task/resource cleanup
and concurrent calls. Factory startup captures the stable backend allocation, not
the address of a Runtime object that has not yet been constructed.

Workers run automatically after successful creation; the caller does not drive
their event loops. `run(task_or_factory)` is a blocking convenience for submitting
one root and consuming its result. It does not close admission or stop workers.
Independent tasks may remain active when it returns; destruction cancels and
drains them. Explicit `shutdown()` is needed only when draining must finish before
leaving scope or releasing borrowed state. Calling run from a running Context is
a fatal contract violation; spawn and asynchronously await the handle instead.

All four submission methods accept an owning `Task<T>`, a nullary factory, or a
factory taking `Context &`. Factories must return `Task<T>` by value. If both
signatures are callable, the context-taking form wins. For example:

```cpp
weave::detach(echo(std::move(client)), [](std::error_code error) noexcept {
  WEAVE_LOG_ERROR("Client: %s", error.message().c_str());
});
auto job = runtime->spawn(compute()); // Transfer an existing lazy task.
auto deferred = runtime->spawn(compute); // Invoke compute on a worker instead.
```

Inside a worker, free `weave::detach(task_or_factory, on_error)` inherits the
execution scope. In worker-affine mode it submits to the current Context, not a
round-robin worker, preserving existing socket affinity. In stealing mode it
creates an independent runtime root, initially queued on the executing worker
but eligible for stealing, even from a pinned or Context-owned parent. The
parent's scheduling choice is unchanged. Member detach/detach_on remain explicit
destinations for external submission or intentional placement. Free detach
outside execution is a fatal contract violation; it never invents a runtime.

A direct task's frame and value parameters are constructed on the submitting
thread; its body runs only after scheduling. A factory executes and constructs
its coroutine on the selected (or stealing) worker. Its optional Context argument
is that worker's Context. Named Task variables require `std::move(task)`.

The runtime owns the task and any factory until completion. Passing a capturing
coroutine lambda as the factory retains its closure. Passing the Task returned by
invoking a temporary capturing coroutine lambda does **not** retain that closure;
prefer a named coroutine with owned parameters or submit the callable itself.
Accepted frames and factory captures are destroyed on an execution worker before
publishing the join result: the assigned worker for affine/pinned tasks, or the
finishing worker for a movable task. Rejected factories and unstarted frames are
destroyed on the submitting thread without running the task body.

Direct submission does not transfer resource affinity. Create sockets in the task
body or factory on a worker. Existing affine sockets require their matching worker
with spawn_on/detach_on; stealing-mode sockets may move only within the same runtime.

## Results and joins

- `run(task_or_factory)` returns `Result<T>`, including void, after the root finishes.
  It reports both rejection and execution failure and can be called repeatedly.
  It waits only for its root, not independently spawned or detached tasks.
- `spawn(task_or_factory)` returns `Result<JoinHandle<T>>`.
- `detach(task_or_factory)` and `detach_on(worker, task_or_factory)` return `void`, without
  `co_await`. They own the task and any factory through completion and discard its
  value on a runtime worker. Both accept an optional final `on_error` argument;
  without it, submission and execution errors are deliberately discarded.
- `Context::spawn(task_or_factory)` uses the same JoinHandle and lifetime machinery, but
  always executes on that context's owner. Inside a runtime it remains context-affine,
  even with work stealing enabled. `Context::detach` is also context-affine.
  Runtime join/shutdown also drain these tasks.
- Every Task is fallible. Socket and nested Task failures propagate automatically;
  `co_await as_result(operation)` yields `Result<T>` for explicit recovery.
- `std::move(handle).get()` waits on a non-runtime thread and consumes the handle.
  It returns `Result<T>`, including `Result<void>` for a void task.
  Blocking run(), get(), join(), shutdown(), or runtime destruction inside a running
  Context is a fatal contract violation; use an asynchronous join instead.
- `co_await std::move(handle)` yields `T` or propagates failure, suspending the
  caller without blocking its worker. Use `as_result(std::move(handle))` to recover.
  Affine/pinned callers resume on their assigned worker; movable callers may
  resume on another worker. Standalone `Context::run` callers still resume on their
  own Context thread. A child's scheduling choice does not change its parent's.
- Handles are move-only and single-consumer. Dropping one does not cancel its
  task. The runtime retains ownership until completion, then releases an unclaimed
  result. Task itself forbids destroying a running or pending computation.

Detach's error handler must return void, be `noexcept`, and be nothrow-movable;
move-only captures and named functions are supported. A rejected submission calls
it synchronously on the submitter, outside the admission lock, without executing
the factory. A failed accepted task calls it on its finishing worker, after frame
cleanup but before releasing factory captures. Handler captures are also reclaimed
on a worker. It is not called on success. An accepted task may finish before the
submitting call returns, and handlers for different tasks can run concurrently.
The usual worker restrictions on blocking, shutdown, and borrowed state apply.
Passing a handler here avoids needing a separate Task::on_error adapter, and also
covers rejection before a task starts. If both observers are explicitly attached,
both observe task failure; neither consumes or recovers from the error.

For example, either scheduler can spawn another task and await it with one worker:

```cpp
auto parent = runtime->spawn([&runtime]() -> weave::Task<int> {
  auto child = runtime->spawn([]() -> weave::Task<int> { co_return 42; });
  if (!child)
    co_await weave::fail(child.error());
  co_return co_await std::move(*child);
});
```

C++ has no Rust Send/lifetime checker. Borrowed captures must outlive the task.
Capture/result types must permit ownership transfer between their relevant
threads. In stealing mode, do not hold thread-owned locks across suspension or
assume thread_local values follow a task. Select worker_affine, spawn_on, or detach_on
for thread-bound resources. Weave's serialization covers its own awaiters; custom
awaiters that directly resume coroutine handles outside the scheduler are not
supported.

In affine mode, TcpStream/TcpListener remain thread-confined. In stealing mode,
their owning task may migrate, and ownership may be transferred between tasks
in the same runtime. A socket still belongs to its original Context/IOCP, not
the current execution worker. A live socket must not escape to the main thread
or another runtime, and its runtime must outlive it.

Asynchronous TCP entry and native submission validate the execution domain,
including empty-buffer operations. A different Context is allowed only when
both belong to the same work-stealing runtime and the current root supplies
runtime continuation routing. Context-owned roots remain affine and must use
their own Context's sockets. Unrelated same-thread contexts fail a contract
rather than hanging on the wrong IOCP. Synchronous setup/cleanup retain their
ordinary thread/group checks and remain usable outside run.

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
Further submissions, including nested calls from existing tasks, are rejected with
operation_canceled: spawn returns the error, while detach reports it to its optional
handler. This also closes Context::spawn/detach on its worker contexts.
Await root tasks before calling join if they still need to
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
uncooperative external work can prevent shutdown. Individual cancellation uses
JoinHandle::cancel() or SpawnOptions with CancelSource/CancelToken. See
[cancellation, scopes and timers](cancellation.md). Runtime must outlive concurrent calls to its methods.
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

All contexts track live socket handles for cancellation. Sharded ports are polled
only by their owner; shared ports are polled by all runtime workers. A completion
records its originating Context, including when another worker dequeues it.
Packets schedule continuations onto the owning task's serialized ready queue in
stealing mode. Affine/Context-owned continuations collected elsewhere enter the
owner's intrusive ready queue, never execute on the collector. Context metrics
use atomic updates in stealing or shared-port mode, and metrics()
returns a snapshot by value. Concurrent fields in that snapshot need not describe
one instant in time. Scheduler wake packets are not socket-completion metrics.

Each joinable task has one shared result/control allocation with two intrusive
references (runtime and consumer), not a shared_ptr per socket operation. Detached
tasks use the same control block with only the execution reference; no temporary
consumer handle is created. Their optional error handler is stored with the
factory and is invoked during deferred completion, without an extra coroutine
wrapper. Ready publication and waiter registration use one
atomic handshake to avoid lost wakes
or duplicate resumes. A stealing dispatch temporarily retains its task while
resumption unwinds, so a result consumer cannot free an executing control block.
Task frames and captures are reclaimed on a valid runtime worker.
The result may be destroyed on a runtime worker or on the thread consuming or
dropping its handle, depending on which reference is released last. With detach,
there is no consumer reference, so the result is reclaimed on a runtime worker.
The coroutine-frame cache remains thread-local; spawning is not allocation-free.
Direct Task overloads transfer an existing frame through a non-coroutine owning
adapter; they do not allocate a second coroutine frame or change the scheduler.

Global submission locking coordinates admission and shutdown. Stealing adds
mutex-protected per-worker queues and an intrusive per-task continuation queue;
there is no heap allocation per queued I/O continuation. Worker deques can
allocate blocks, and task control blocks/frame caches still allocate as needed.
Ordinary affinity completions do not take scheduler queue locks.

An idle worker publishes its idle state before rechecking ready queues; enqueuers
wake it through its own IOCP in sharded mode, or a targeted no-op user APC in
shared mode. Shared workers use alertable GetQueuedCompletionStatusEx waits;
the APC only interrupts the wait, never invokes user tasks. This avoids a second
driver thread, polling timeouts, or packets repeatedly consumed by the wrong
affine worker. Local ready publication uses a mutex and wake-on-empty transition;
the owner rechecks it before parking. This closes the enqueue/park lost-wakeup race.
Workers alternate pinned/movable local work and service I/O after at most 64
ready dispatches. Active roots and dispatches must both drain before contexts
are destroyed. This is a correctness-first stealing baseline, not a lock-free
scheduler or a strict fairness/tail-latency guarantee.
