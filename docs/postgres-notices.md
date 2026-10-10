# PostgreSQL notices

Notice and warning messages do not fail a query. By default, Weave retains them
in a bounded owning queue drained with `take_notices()`. An optional handler
receives them inline instead:

```cpp
auto database = co_await weave::pg::connect(options,
  [](const weave::pg::Diagnostic &notice) noexcept {
    auto message = std::string{notice.message()};
    WEAVE_LOG_INFO("Postgres: %s", message.c_str());
  });
```

Include `<weave/postgres.hpp>` and `<weave/log.hpp>` for this example. The handler
is installed before startup, so login notices are observable even when startup
subsequently fails. The diagnostic overload is
`connect(options, diagnostic, handler)`; the caller-owned startup diagnostic
still describes a fatal server error, not the last notice. Both
`BlockingConnection::connect` overloads accept the same optional handler.

## Replacement

`NoticeHandler` is `std::move_only_function<void(const Diagnostic &) noexcept>`.
It accepts move-only captures, requires a `noexcept` callable and owns the registered
callable. `on_notice` is synchronous and returns `Result<NoticeHandler>` containing
the previous callable, allowing explicit save/restore:

```cpp
auto previous = database.on_notice(
  [](const weave::pg::Diagnostic &notice) noexcept {
    auto message = std::string{notice.message()};
    WEAVE_LOG_ERROR("Postgres: %s", message.c_str());
  });
if (!previous)
  co_await weave::fail(previous.error());

co_await database.query("DO $$BEGIN RAISE NOTICE 'hello'; END$$");

auto restored = database.on_notice(std::move(*previous));
if (!restored)
  co_await weave::fail(restored.error());
```

Passing an empty handler (`on_notice({})`) restores the default queue policy.
Previously queued notices are preserved; registration does not replay or discard
them. Replacement is permitted only while the connection is idle with no deferred
operation or pipeline/exchange lease. Otherwise it returns `Error::busy` without
replacing the existing registration. A rejected incoming callable is destroyed.
The same synchronous API exists on `BlockingConnection`.

## Execution And Ownership

The handler runs during message parsing on the thread currently executing the
connection's coroutine graph. A standalone Context has one owner thread; a
work-stealing Runtime may execute that graph on different workers over time.
There are no callback threads. Connection is still a single-execution-owner
object: independent roots or external threads must not concurrently access it.

The `Diagnostic` reference and any views into it expire when the callback returns.
Copy the diagnostic to retain it, and bound any application-side storage. A
callback must return promptly and cannot suspend. Do not start session operations,
recursively drive a Context/blocking facade, or mutate the connection inside it.
Reentrant handler replacement, `close()` and `cancel()` return `Error::busy`.
Inspecting/copying the received diagnostic needs no access to Connection.

Captures must own their data or have a lifetime covering every possible callback.
Returning a previous handler transfers that responsibility to its new owner;
discarding it destroys its captures. Query results do not retain callbacks.
Connection move and successful or failed reset preserve the registration. During
reset, notices from replacement startup use the same receiver, even before a
replacement Connection is published. Closed idle connections may replace the
registration before another reset.

Every notice is structurally validated and subject to the message-size limit
before delivery. Malformed/oversized messages fail the operation and close the
session without invoking the handler for that message. Callback mode does not
retain notices against Weave's queue budget; default mode still fails on overflow
rather than silently dropping them. Installing a handler does not bypass limits
for notifications, query results or other wire messages.

## Compatibility

This maps the structured part of [libpq notice processing](https://www.postgresql.org/docs/18/libpq-notice-processing.html)
to an owning C++ callback. Weave deliberately has one handler layer and queues by
default instead of printing to stderr. Empty registration clears the handler,
whereas a null libpq receiver inspects without replacing. Weave callbacks cover
server notices only: local failures remain `Result`/`Task` errors, and accessing
owning results never invokes an old connection handler. Notifications have a
separate [consuming callback](postgres-notifications.md); owning
[lifecycle hooks](postgres-events.md) have separate ownership contracts. Protocol tracing uses a
separate [owning observer](postgres-tracing.md).
