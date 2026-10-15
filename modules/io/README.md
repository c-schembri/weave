# Execution

Weave uses C++23 coroutines without exceptions. Async operations return `Task<T>`;
synchronous operations return `Result<T>` (`std::expected<T, std::error_code>`)
when they can fail.

## Tasks And Errors

```cpp
weave::Task<void> echo(weave::TcpStream client)
{
  std::array<std::byte, 4096> buffer;

  while (auto received = co_await client.read(buffer))
    co_await client.write_all(std::span{buffer}.first(received));
}
```

An awaited Task produces its value or propagates failure without running the
rest of the enclosing coroutine. Recover only where needed:

```cpp
auto result = co_await weave::as_result(operation());
if (!result)
  WEAVE_LOG_ERROR("Operation: %s", result.error().message().c_str());
```

Synchronous Results are **not awaitable**. Check them explicitly; inside a Task,
`co_await weave::fail(result.error())` propagates their failure. This routing
awaiter also preserves failed Results whose error-code value is zero.
Non-void Tasks additionally accept `co_return std::unexpected(error)`.

`task.on_error(noexcept_handler)` observes task failure after frame cleanup,
without recovering or changing the result. It does not observe submission rejection.
Foreign awaiters do not acquire automatic propagation/cancellation semantics;
foreign coroutine promises must consume Weave Tasks through `as_result`.
Contract violations and allocation failure terminate, rather than becoming
ordinary operational error codes.

## Context

Include `<weave/io.hpp>` and link `weave::io`:

```cpp
auto ctx = weave::Context::create();
if (!ctx)
  return weave::report_error(ctx.error());

auto result = ctx->run(operation());
if (!result)
  return weave::report_error(result.error());
```

A Context is a single-owner execution unit driven on its creating thread.
`run(task)` drives it until that root completes and returns `Result<T>`.
It does not join independently submitted work or shut down admission.
`run()` without a task serves submissions until `request_stop()`, then drains.
External runtimes can drive Contexts directly; Weave Runtime is optional.

Contexts and their owning factory Results are immovable. Keep them alive until
all sockets, borrowed Tasks and pending work have been destroyed/drained.
An executing scope is established by the driver, not merely by constructing a
Context. Two Contexts on one thread are not interchangeable I/O destinations.

## Runtime

Include `<weave/runtime.hpp>` and link `weave::runtime`:

```cpp
auto runtime = weave::Runtime::create({
  .workers = 4,
  .scheduler = weave::Scheduler::work_stealing,
});
if (!runtime)
  return weave::report_error(runtime.error());

auto result = runtime->run(operation());
```

Runtime owns worker threads and their Contexts. `workers = 0` selects the
hardware-concurrency count, falling back to one. `worker_affine` keeps a root on
its selected worker; `work_stealing` allows idle workers to take runnable roots.
Execution within a root remains serialized, including directly awaited children.
Independent roots can run concurrently. CPU-bound code must yield/cooperate.

`run(task_or_factory)` submits to workers and blocks the caller until that root
finishes. Never call it from an executing Context; await a spawn handle there.
Like Context's overload, it does not join independent work. `spawn_on` and
`detach_on` explicitly pin submissions to a worker. Sockets retain their original
I/O binding when movable roots migrate.

Scheduling and I/O layout are separate options. Sharded I/O is the default.
Shared IOCP is an opt-in Windows layout; Linux returns `operation_not_supported`
for shared layout, not an implicit fallback. [Measured tradeoffs](../runtime/benchmarks/README.md).

`join()` closes admission and drains without requesting cancellation.
`request_stop()` requests cancellation; `shutdown()` also joins. Destruction
cancels and drains. Runtime and its owning Result are immovable.

## Submission And Ownership

| Operation | Result | Ownership |
| --- | --- | --- |
| `ctx.spawn(task_or_factory)` / Runtime equivalent | `Result<JoinHandle<T>>` | Executor owns execution; caller can join/cancel |
| `ctx.detach(...)` / Runtime equivalent | `void` | Deliberately unjoined; executor retains execution |
| `weave::detach(...)` inside a Task | `void` | Selects that Task's execution scope and submission policy |

Awaiting a join handle produces `T` or propagates failure. Use
`co_await as_result(std::move(handle))` for recovery, or
`std::move(handle).get()` outside an executing Context for a blocking Result.
Dropping the handle neither cancels nor destroys active execution.

Detach's optional `noexcept` error handler reports rejection synchronously on
the submitter and execution failure on the executor after cleanup. Without a
handler, errors are intentionally discarded. Free `detach` outside an executing
scope is a fatal contract violation; choose an explicit destination there.

Factories return Tasks by value and may accept `Context &` or no arguments.
They construct Tasks on the executor and remain alive through completion.
Direct Tasks are constructed on the caller and run later; rejected submissions
destroy them on the caller. Prefer named coroutines with owning value arguments.
Invoking a temporary capturing coroutine lambda does **not** make its Task own
the lambda closure.

Moving/destructing active Tasks or streams is a contract violation. Join every
borrower before destroying its Context, stream, buffer or referenced arguments.
Keep Runtime-bound sockets and pools inside compatible execution scopes rather
than returning them to an unrelated caller thread for destruction.

## Cancellation

```cpp
weave::CancelSource stop;
auto job = runtime->spawn(operation(), {.cancel = stop.token()});
if (!job)
  return weave::report_error(job.error());

stop.cancel();
auto result = std::move(*job).get();
```

`JoinHandle::cancel()` requests cancellation of one job. CancelSource copies
share state; tokens remain valid independently. Do not consume/move a handle
concurrently with calling its members.

Direct awaits and `when_all` children inherit their parent's token. Independent
spawn/detach do not: pass a token or use a scope. Awaiting an independent join
handle does not cancel that job on the waiter's behalf; a started join drains.
Accepted precancelled submissions do not invoke their factory/body.

Cancellation is cooperative, not forced destruction, rollback or proof that no
bytes were sent/consumed. Native completion can win the race. Cancel, then drain
before releasing borrowed memory. Whole-socket `cancel()` remains separate from
per-operation task cancellation. CPU loops need `cancellation_point()` and
scheduling fairness through `ctx.yield()`; yielding alone is not a cancellation point.

## Structured Work

`when_all(...)` joins every child, then propagates an error. It does **not**
cancel siblings on first failure; an unending sibling still prevents completion.

`weave::scope(body)` owns a TaskScope. Its body receives `TaskScope &` and can
spawn children. Normal exit joins; body failure/cancellation cancels and drains
children before propagating. Unhandled child failure propagates after draining;
child failure alone is not fail-fast cancellation. `scope.cancel()` requests
cancellation and `scope.join()` closes admission and joins.

C++ destroys successful coroutine body locals before final suspension. A scope
wrapper cannot extend them. Give children owned arguments, borrow from an owner
outside the body, or explicitly join before a body-local owner leaves scope.
Never destroy a manually constructed TaskScope with active children.

## Timers

Include `<weave/timer.hpp>`:

```cpp
using namespace std::chrono_literals;

co_await weave::sleep_for(250ms);
auto received = co_await weave::timeout(5s, client.read(buffer));
```

`sleep_until` and `timeout_at` accept steady-clock deadlines. Relative deadlines
start when the lazy Task executes. Nonpositive delays/past deadlines complete
immediately unless cancelled; NaN fails and oversized delays saturate.
There is no busy-polling or separate timer thread.

Timeout reports `timed_out`; parent cancellation reports `operation_canceled`.
Both cancel and drain the losing operation/timer before returning. An immediate
operation starts first and can win a zero timeout. Deadlines are not hard wall-clock
bounds: scheduling, blocking code or a foreign noncancellable operation can delay
safe draining. Windows waits round up to milliseconds.
