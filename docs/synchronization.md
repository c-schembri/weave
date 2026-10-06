# Channels And Semaphores

Include `<weave/sync.hpp>` and link `weave::sync`, or include only
`<weave/channel.hpp>` / `<weave/semaphore.hpp>`. Sync depends on IO, not TCP or Runtime.
The same primitive can connect tasks on one Context, independent Context threads,
or a multicore Runtime. Waiting suspends the task; it does not block a worker.

## Bounded Channels

```cpp
weave::Channel<Message> messages(64);

co_await messages.send(std::move(message));
auto received = co_await messages.receive();
if (!received)
  co_return; // Closed and drained.
```

Capacity must be positive. Sends wait when full; receives wait when empty.
`send(T)` owns its argument while suspended. Values may be move-only and need not
be default-constructible, but moving and destroying them must be nonthrowing.

`close()` is synchronous, thread-safe and idempotent. It rejects pending/future
sends with `SyncError::closed`, wakes receivers, and preserves buffered messages.
After those drain, `receive()` returns `std::nullopt`. Closure is not cancellation.

`try_send(T &)` and `try_receive()` are synchronous `Result` APIs. Full/empty
returns `resource_unavailable_try_again`; a failed `try_send` leaves its argument
unmoved. A drained closed `try_receive` succeeds with `std::nullopt`.

Suspended senders and receivers queue FIFO. Delivery or cancellation wins while
holding the channel's state lock. A value already delivered is not rolled back
if cancellation arrives afterward. New async waits observe pre-cancellation.

## Semaphores

```cpp
weave::Semaphore limit(32);

auto permit = co_await limit.acquire();
co_await process_request();
// The owned permit is released on success, failure, or cancellation cleanup.
```

Initial capacity must be positive. `acquire()` waits fairly behind existing FIFO
waiters; `try_acquire()` is synchronous and returns `resource_unavailable_try_again`
if none are available. A move-only `Permit` returns capacity on destruction.
`permit.release()` releases early and is idempotent; moved-from permits are inert.

`close()` rejects pending/future acquisitions with `SyncError::closed`. It does
not revoke outstanding permits. Destroy the semaphore only after every permit
has been released and every waiting task has drained.

## Cancellation And Ownership

Task-token or Context-stop cancellation unlinks pending waits and reports
`operation_canceled`. Permit grants and message delivery can win the race. Stop
callbacks are disarmed before coroutine waiter storage is reclaimed, and wakeups
go through the waiting task's captured Context/executor, preserving affinity.

Channel and Semaphore are immovable. Their owning objects must outlive every
task borrowing them. `close()` wakes waiters but does not join them: join handles
or a TaskScope before destroying the primitive. Destruction with pending waiters
(or outstanding semaphore permits) is a fatal contract violation.

The channel preallocates its bounded ring. Wait records live in coroutine frames;
there is no separately allocated wait node or per-message library allocation.
This is not a zero-allocation guarantee: tasks, payloads, and initial storage may
allocate. Allocation failure follows Weave's terminating, exception-free policy.

[Pipeline example](../modules/sync/examples/pipeline.cpp) /
[Cancellation](cancellation.md) / [Task lifetimes](tasks.md).
