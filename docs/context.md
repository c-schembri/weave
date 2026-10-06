# Context execution

`#include <weave/io.hpp>` and link `weave::io`. Context owns an event loop and
independent tasks, but creates no thread and has no dependency on Runtime or TCP.
Create it on the thread that will drive and destroy it. Context is immovable;
spawning from another thread does not transfer the context itself.

```cpp
#include <weave/io.hpp>
#include <weave/log.hpp>

auto ctx = weave::Context::create();
if (!ctx)
  return weave::report_error(ctx.error());

auto job = ctx->spawn([](weave::Context &ctx) -> weave::Task<int> {
  co_await ctx.yield();
  co_return 42;
});
if (!job)
  return weave::report_error(job.error());

auto result = ctx->run(std::move(*job).as_task());
if (!result)
  return weave::report_error(result.error());
```

## Driving work

- `run(task)` drives the calling thread until that root task completes and returns
  `Result<T>`. It does not wait for independently spawned tasks. Join those tasks
  explicitly before releasing anything they borrow, or give them owned data.
- `run()` is a persistent service loop: it waits for submissions even while idle.
  After `request_stop()` it drains all context-owned tasks and returns. There is
  no background execution when the context is not being driven.
- `spawn(task)` and `spawn(factory)` are thread-safe and return `Result<JoinHandle<T>>`.
  The task body or factory starts later on that context's owning thread. Admission
  after stop returns `std::errc::operation_canceled`. Out-of-memory remains fatal.
- `detach(task)` and `detach(factory)` have the same execution and ownership rules
  but return `void`, without a join handle or `co_await`. Adding `on_error` reports
  submission rejection and task failure through an optional synchronous,
  `noexcept` handler returning void. It may own move-only captures and must be
  nothrow-movable. Omission deliberately discards errors. Detached values are
  discarded on the context's owning thread; submission never joins the task.
- `request_stop()` is thread-safe, closes admission, and wakes the loop. The owner
  requests cancellation for registered I/O, including sockets created before run.
  It affects the entire context, not one task. Use Runtime::request_stop() to
  coordinate whole-pool cancellation when using the built-in runtime.
- `shutdown()` requests stop and drains owned tasks on the owning thread. Context
  destruction does the same. Neither may recursively drive a running context.
  Stop is terminal; it does not provide a restart/reset operation.

`run(task)` does not stop the context when its root succeeds or fails. A subsequent
run can drive another root and existing owned tasks. If an accept loop fails while
sessions remain active, destruction cancels and drains them automatically, as in
the [buffered context echo server](../modules/tcp/examples/echo/minimal/context.cpp)
or the [explicit stream version](../modules/tcp/examples/echo/stream/context.cpp).
Use explicit `shutdown()` when draining must finish before leaving the scope,
especially before destroying data borrowed by those tasks.

When also using the TCP module, `co_await tcp::listen(ipv4, port)` and
`co_await tcp::connect(ipv4, port)` resolve the active Context automatically.
`run()` and `run(task)` establish this execution scope, including when driven
by an external runtime. The setup Tasks are lazy: construction and destruction
of unstarted Tasks require no active Context. Execution without one is a fatal
contract violation, not a request to create an event loop. Address strings are
borrowed until setup finishes. Use `tcp::listen(ctx, ...)` for synchronous setup
before run; that overload returns `Result<TcpListener>`. Socket operations keep
using their stored Context rather than selecting a new one on each await.

Before starting asynchronous TCP setup or operations, the executing scope must
match the bound Context, or route continuations through the same work-stealing
runtime. A matching thread ID is not enough. Two standalone Contexts may coexist
on one thread and be driven separately, but driving B while awaiting A's socket
fails a contract rather than polling the wrong port indefinitely. Context-owned
tasks remain affine: cross-Context socket use requires runtime-routed execution.
Explicit synchronous listener setup and socket cleanup still work outside run.

## Submission forms

Inside an executing task, `weave::detach(task_or_factory, on_error)` selects the
current submission scope. Standalone Context drivers, including custom runtimes
using public Context APIs, inherit local ownership. The function is provided by
`<weave/io.hpp>` or `<weave/io/detach.hpp>`, returns void and requires no await.
There is no active scope merely because a Context has been created. Calling free
detach outside execution is a fatal contract violation; use an explicit member
to submit from outside or target a different executor. Optional error handlers
and task/factory lifetimes follow the member API's rules.

Built-in worker-affine runtime scopes also keep children on the current Context.
Work-stealing scopes instead submit independent, stealable runtime roots, even
when the caller is a Context-owned task. `ctx.detach(...)` remains explicitly
Context-affine in either runtime mode. Detached children never join their parent
automatically or extend its borrowed state.

Both spawn and detach accept:

- An owning `Task<T>`: `ctx.detach(echo(std::move(client)), on_error)`.
  A named Task variable must be passed with `std::move(task)`.
- A nullary factory returning `Task<T>` by value: `ctx.spawn(compute)`.
- A factory taking `Context &` and returning `Task<T>` by value, as above.
  If a callable supports both signatures, the context-taking form wins.

Calling a coroutine creates its frame and transfers value parameters immediately;
its body is lazy. Direct submission transfers that frame without an extra wrapper
coroutine. A factory instead runs and constructs its task on the execution thread,
and is not invoked at all if rejected. Use factories when construction must happen
on the context thread. Direct submission does not relax socket/thread affinity.

## Ownership and joins

The context retains each accepted task and any factory until completion, whether
submitted with spawn or detach.
Discarding a JoinHandle discards interest in the result, not the task. Completed
coroutine frames and factory captures are reclaimed on the owner before publishing
the join result. Cleanup is deferred until the resuming call has unwound, including
immediate failures. A rejected factory or unstarted task frame is destroyed on the
submitting thread; the task body never runs.

Detach error handlers run once for a rejection or a failed task, never for success.
Rejection invokes the handler before detach returns, on the submitting thread,
without holding an admission lock. Accepted tasks report errors on the context's
owner after their coroutine frames are reclaimed, while factory captures remain
alive. Handler captures and factory captures are then reclaimed on that owner.
An active context on another thread can finish before detach returns; handlers
must allow this timing. Do not block an execution thread or destroy its context
from a handler. Borrowed handler captures must outlive the submission/task.
`Task::on_error` observes only a task that actually starts, not a rejected submission;
use detach's handler when both kinds of error matter.

`co_await std::move(handle)` suspends the caller and propagates the result or error.
Use `as_result(std::move(handle))` to recover. Joins can cross contexts: the waiter
resumes on its own context. `std::move(handle).get()` blocks a non-executing thread;
it does not pump an event loop. Calling get() on the only thread that could execute
the job would deadlock. Use `ctx->run(std::move(handle).as_task())` instead.

Factories may safely own captured coroutine lambdas. A Task returned by invoking
a temporary capturing coroutine lambda does **not** own that lambda's closure;
submit the callable itself, or use a named coroutine with owned value parameters.
Borrowed captures, reference parameters, and root locals are not automatically
extended by spawn or detach. A parent returning or failing
does not join its detached children. Sockets, buffers, and any referenced context
must remain alive until all borrowers and pending operations are finished. Values
that contain context-bound resources must not outlive that context or be destroyed
on an invalid thread. Ordinary independent values can outlive the context in a handle.

Cancellation is cooperative, not forced frame destruction. Accepted tasks and
factories may be cancelled before entry during shutdown. Native I/O and timers are
cancelled and drained. CPU work must finish or reach a cancellation point, and an
outstanding join still needs its child to finish. Individual tasks can be cancelled
without stopping the Context; see [cancellation, scopes and timers](cancellation.md).
Uncooperative work can prevent shutdown from returning. Awaiters and buffers remain
alive until OS completion packets have been drained. The destructor still rejects
externally owned live sockets or other undrained native work.

## Custom runtimes

Thread selection, thread creation, submission distribution, and lifetime coordination
belong to the coordinator, not Context. An external runtime can:

1. Create each Context inside its chosen worker thread.
2. Publish a lifetime-protected reference and drive `run()` on that worker.
3. Route tasks or factories through thread-safe `spawn()` for joins or `detach()` for
   independent work with no join handle.
4. Stop admission, call `request_stop()`, and join its threads, excluding submissions
   before the worker destroys its context.

The [custom-runtime tests](../modules/io/tests/custom_runtime.cpp) implement this
using only public IO APIs and standard C++ threads. That target links `weave::io`,
not `weave::runtime`; installed IO-only consumers also exercise spawn, detach, and joins.

These public APIs support context-affine execution. Cross-context continuation
migration is not a public scheduler plug-in API: the built-in Runtime's optional
work-stealing policy uses an internal execution hook and shared ownership domain.
Do not drive one context from multiple threads, transfer live sockets between
independent contexts, or include private detail headers to imitate that policy.
