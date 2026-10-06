# Cancellation, Scopes And Timers

Cancellation is a cooperative request, not forced destruction or rollback.
Participating operations return `std::errc::operation_canceled`, which propagates
through `Task<T>` normally. Use `as_result()` to recover. Native completions can
win a cancellation race; successfully completed work is not retroactively undone.

## One Task Or A Group

```cpp
auto job = runtime->spawn(serve());
if (!job)
  return weave::report_error(job.error());
job->cancel();
auto result = std::move(*job).get();
```

`JoinHandle::cancel()` is thread-safe and requests cancellation without waiting.
Join/get still observes the eventual outcome. Do not move, destroy, or consume
the handle concurrently with calling its members. Dropping a handle does not
cancel the execution owner's task.

```cpp
weave::CancelSource stop;
runtime->detach(serve(), {.cancel = stop.token()}, log_error);
runtime->detach(background_work(), {.cancel = stop.token()}, log_error);
stop.cancel();
```

`CancelSource` copies share one cancellation state; cancellation is idempotent.
Its token survives the source. An empty `CancelToken` never requests cancellation.
`SpawnOptions` works with Context/Runtime spawn, spawn_on, detach, detach_on, and
free `weave::detach`, for tasks and factories. Accepted precancelled submissions
do not invoke their factory/body; execution ownership still reclaims their captures
and publishes a cancellation result. Detached error handlers also report this case.

Directly awaited tasks and `when_all` children inherit the enclosing task's token.
Independent spawn/detach does **not** inherit that token: pass a token explicitly
or use a scope. Awaiting a JoinHandle does not turn an independent task into a
child or cancel it on the waiter's behalf. A join already in progress drains to
the child's completion; it is not an interruptible wait. If a cancelled parent
never starts its join adapter, the handle is released, not the independent task.

Context/Runtime stop also requests cancellation of their owned roots and pending
timers, closes admission, and drains accepted work. Factories accepted before stop
can now be cancelled before entry. This is cancellation, not an orderly join.
Use Runtime::join() to drain without requesting cancellation, and join Context
handles before shutdown if their successful completion is required.

CPU code must cooperate:

```cpp
for (;;) {
  co_await weave::cancellation_point();
  do_some_work();
  co_await ctx.yield();
}
```

The cancellation point checks the current promise without allocating a frame
or yielding. Yield supplies scheduling fairness; it alone is not a cancellation
point. Foreign awaiters do not become cancellable automatically.

## Native I/O

Task cancellation targets `CancelIoEx(socket, &operation.overlapped)`, not every
operation on that socket. A timed-out read does not cancel a sibling write.
The record, socket, and borrowed buffers stay alive until IOCP dequeues completion.
Stop callbacks are embedded in suspended frames; their destruction synchronizes
with any racing cancellation callback before the record/socket can be reclaimed.
The event loop, not the cancelling thread, resumes the task.

`TcpStream::cancel()` / `TcpListener::cancel()` remain explicit whole-socket
cancellation requests. They do not join. Their native error is not a generic
task-token cancellation unless the task/context has also been cancelled. A
cancelled write may have transmitted bytes, and a cancelled read may have consumed
bytes. Callers must account for partial side effects; cancellation is not a transaction.

## Structured Scopes

`tcp::serve()` uses structured client ownership: cancelling the server cancels and
drains its accepted clients before returning. Unlike an accept loop with `detach`,
its clients never outlive the server task. See [TCP servers](tcp.md).

Include `<weave/scope.hpp>` and link `weave::io`:

```cpp
weave::Task<void> batch(weave::TaskScope &scope)
{
  auto first = scope.spawn(work_a());
  if (!first)
    co_await weave::fail(first.error());
  auto second = scope.spawn(work_b());
  if (!second)
    co_await weave::fail(second.error());
  co_await std::move(*first);
  co_await std::move(*second);
}

// Inside a Task:
co_await weave::scope(batch);
```

The wrapper retains the callable and owns the scope. It joins every child before
returning, even if handles were discarded. A failing/cancelled body requests child
cancellation and drains them before propagating the body error. Otherwise, an
unjoined child error is propagated after all children drain; first observed error
wins. Child failure alone is not fail-fast sibling cancellation. Recover inside
the child if its failure should not fail the scope.

`TaskScope::cancel()` requests cancellation; `join()` closes scope admission and
drains without allowing cancellation to bypass cleanup. The scope's direct body
and children share its token. Children inherit the executor's submission policy,
including independent stealable scheduling inside a work-stealing Runtime.

**C++ lifetime boundary:** successful `co_return` destroys ordinary body locals
before final suspension. The wrapper cannot prolong those locals. Give children
owned parameters, borrow from the enclosing caller/callable (which must outlive
the scope), or explicitly await all borrowers before a body-local object goes out
of scope. A failed body frame is retained until child draining completes, but that
does not repair a local that already left its lexical scope. C++ has no borrow
checker here. Do not construct a manual TaskScope and destroy it with active children.

## Awaitable Timers

Include `<weave/timer.hpp>` and link `weave::io`:

```cpp
using namespace std::chrono_literals;

co_await weave::sleep_for(250ms);
co_await weave::sleep_until(std::chrono::steady_clock::now() + 1s);
auto bytes = co_await weave::timeout(5s, client.read(buffer));
```

`sleep_for` and `sleep_until` return lazy `Task<void>`. Relative time starts when
the task executes, not when it is constructed. Negative/zero delays and past
deadlines complete immediately unless cancelled. Timers use steady-clock deadlines;
they do not promise real-time precision. Windows waits are rounded up to milliseconds,
and scheduler load can delay resumption. Floating durations are supported, NaN fails
with `invalid_argument`, and oversized/infinite delays saturate at the maximum deadline.

Timers use an indexed deadline heap and the nearest deadline as the IOCP wait
timeout. There is no periodic polling, busy-wait, or extra timer thread. Shared
IOCP uses a domain-wide queue and targeted APC wakes so sleeping collectors update
their wait when an earlier deadline is inserted. Continuations still follow the
same affine/serialized-executor routing as socket completions.

`timeout(duration, task)` and `timeout_at(steady_deadline, task)` return `Task<T>`.
The first observed operation/deadline completion wins; an immediate operation is
started first and can win even with a zero timeout. Timeout reports
`std::errc::timed_out`. Cancellation of the wrapper reports `operation_canceled`.
Both cases cancel and **drain** the losing operation/timer before returning.

```cpp
auto result = co_await weave::as_result(weave::timeout(5s, client.read(buffer)));
if (!result && result.error() == std::errc::timed_out) {
  // The read has finished cancellation; its buffer is no longer kernel-owned.
}
```

A deadline is not a hard upper bound: a CPU loop, blocking function, or foreign
awaiter that ignores cancellation can delay or prevent draining. Unlike simply
dropping a Rust future, destroying a pending IOCP coroutine would leave the kernel
with dangling memory. Weave deliberately waits for safe completion.
