# PostgreSQL Notifications

PostgreSQL subscriptions are ordinary SQL. For a dedicated notification session,
the simplest API needs no callback:

```cpp
co_await database.query("LISTEN app_events");

for (;;) {
  auto event = co_await database.wait_notification();
  WEAVE_LOG_INFO("Event %s: %s", event.channel.c_str(), event.payload.c_str());
}
```

`wait_notification()` returns an owning `Notification` with the source backend
process ID, channel and payload. It first drains already queued events, then
awaits input without polling. It is an exclusive session reader: do not execute
queries, pipelines or COPY on that connection while it is pending. Prefer a
dedicated connection when subscriptions must be processed continuously.

## Optional Callback

Use a callback when notifications also need observing during other exchanges:

```cpp
auto previous = database.on_notification(
  [](const weave::pg::Notification &event) noexcept {
    WEAVE_LOG_INFO("Event %s: %s", event.channel.c_str(), event.payload.c_str());
  });
if (!previous)
  co_await weave::fail(previous.error());

co_await database.query("LISTEN app_events");
co_await database.query("SELECT 42");

auto restored = database.on_notification(std::move(*previous));
if (!restored)
  co_await weave::fail(restored.error());
```

Registration is synchronous and returns `Result<NotificationHandler>` containing
the previous owning, move-only callable. Empty registration restores the default
bounded queue. `BlockingConnection` exposes the same registration API and returns
`Result<Notification>` from its blocking `wait_notification()`.

Callbacks are **consuming**, not an additional unbounded queue. Each newly
decoded, structurally valid NotificationResponse invokes the installed handler
once instead of being queued. Query, batch, pipeline, row and COPY processing
can all receive these administrative messages. Per-message wire bounds and
validation still apply before invocation. With no handler, retained notification
count/bytes remain bounded; overflow fails rather than silently losing events.

An explicit idle `wait_notification()` still returns the newly decoded event to
its caller **and** invokes the installed callback once. This deliberate exception
prevents a consuming handler from swallowing the event and leaving the wait
stuck. Do not process the same event twice unintentionally. Draining a previously
queued event does not invoke a newly installed handler: registration neither
replays nor discards the existing backlog. The registration at decoding time
applies, not the time a peer originally sent or the socket buffered the bytes.

## Ownership and Execution

The handler owns its captures; its `const Notification &` argument is borrowed
only through invocation. Copy data intentionally retained after the callback.
Delivery is inline on the executing connection graph, not on a helper thread.
Notification handlers use the session's single-reader serialization; different
connections or different observers are not mutually serialized. Synchronize
shared captured state yourself.

Keep callbacks short and `noexcept`. Do not drive nested Context/Runtime loops
or reenter session operations. Registration changes reject deferred/active Tasks,
pipelines and other session borrows with `Error::busy`; reentrant cancellation
also rejects while the handler is invoked. Metadata access is not permission
to mutate a Connection concurrently. Callback support does not make a session
generally thread-safe.

Observers survive connection moves and successful or failed resets, including
startup messages received during reset attempts. Initial connect has no
notification-handler argument; any notifications decoded before registration
use the default queue. Reset replaces server session state and its queued data;
it does **not** reissue LISTEN. Subscribe again explicitly on the new session.

No handler creates a background reader or changes PostgreSQL transaction
delivery rules. Delivery happens only while an operation consumes input. A
successfully received event can win a concurrent cancellation request; failed
or cancelled pending reads close/drain the session before reuse. Reset and
subscribe again if reuse is needed after cancellation.

Treat event payloads as application data, not SQL or trusted identity assertions.
The source process ID is a reported backend identifier, not an authorization
principal. Logging policy and any copied payload retention belong to the application.

## Tests and Mapping

`weave_postgres_notifications` uses owned peers to cover queue/callback
boundaries, old backlog, deferred Tasks, save/restore, reset failure, receiver
lifetime, malformed/over-limit query and idle messages, cancellation, competing
waits, pipeline and duplex COPY. Runtime-enabled builds include four-worker
schedulers and Windows shared IOCP. The optional
`weave_postgres_notifications_live` exercises real LISTEN/NOTIFY with SCRAM and
verified mTLS, blocking operation, reset/resubscription and 16 simultaneous
observed sessions per four-worker scheduler.

[libpq's notification API](https://www.postgresql.org/docs/18/libpq-notify.html)
returns messages already consumed by other operations through `PQnotifies`;
it does not provide this owning callback API. Weave's bounded queue and owning
wait map that capability, while callbacks are an optional ergonomic extension.
See the [qualification record](postgres-qualification.md) for configurations
actually run; these controls do not prove arbitrary deployment or exhaustive races.
