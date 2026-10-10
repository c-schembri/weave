# PostgreSQL Lifecycle Events

The original connection/server-result lifecycle API passes the frozen Windows/Linux Debug, Release
and ASan correctness matrix, reduced-module builds and relocated consumers.
See the [qualification record](postgres-qualification.md#lifecycle-event-qualification-2026-10-08)
for the tested scope and remaining release limits.
Application-built attachment has its own qualification scope recorded below.

Lifecycle observers associate owning application state with connections and
results. They are separate from notices, notifications and protocol tracing.

```cpp
auto key = database.on_event(
  "command",
  [](weave::pg::Event &event) noexcept -> weave::Result<void> {
    if (event.kind == weave::pg::EventKind::result_create)
      event.data = std::make_shared<std::string>(event.result->command);

    if (event.kind == weave::pg::EventKind::result_copy) {
      auto source = std::static_pointer_cast<std::string>(*event.source_data);
      event.data = std::make_shared<std::string>(*source);
    }

    return {};
  });
if (!key)
  co_await weave::fail(key.error());

auto result = co_await database.execute("SELECT 42");
auto data = result.event_data(*key);
if (!data)
  co_await weave::fail(data.error());

auto command = std::static_pointer_cast<std::string>(*data);
```

The registration owns a move-only `noexcept` callable. Its nonempty name is
copied and must be unique on that connection. `EventId` identifies the
registration independently of object addresses and survives connection moves
and resets. Several registrations can coexist. There is no implicit replacement
or unregistration; the callable must remain available to retained results.

`EventData` is `std::shared_ptr<void>`. Use ordinary owning types, casts and
custom deleters rather than borrowed opaque allocations. Captures replace
libpq's pass-through pointer; connection and result data are separate slots.
The application selects their actual types and must not cast the wrong type.

## Delivery

| Kind | Timing and borrowed arguments |
| --- | --- |
| `registered` | Synchronous admission; `connection` is valid and `data` starts empty |
| `connection_reset` | After successful session replacement; existing connection `data` is retained |
| `connection_destroy` | Logical Connection destruction, before its implementation is torn down |
| `result_create` | A validated complete result or pipeline chunk; `connection` and `result` are valid, `source_data` refers to connection data, and result `data` starts empty |
| `result_copy` | After selected payload fields and all scalar/command/parameter metadata have been copied; `source` and destination `result` are valid, `source_data` refers to the source's result data, and destination `data` starts empty |
| `result_destroy` | Before result fields and owning data are released; `result` and its `data` are valid |

`connection` is null for result copy/destruction: a result may outlive the
connection. Each result retains only successful observer registrations and its
own data, not an implicit connection/data-registry owner. Captured application state
can deliberately live until the last observed result is released.
Avoid owning capture cycles and do not retain borrowed Connection pointers past
their lifetime; the library cannot extend or collect application-created ownership.

Moves transfer result registrations and data without copy/create/destroy events
for the moved-from object. Copy construction and copy assignment invoke copy
hooks; copy assignment first destroys the destination's old registrations.
Move assignment destroys the old destination's registrations, then transfers
the source's. Self-assignment does not emit events. A copy's data is not shared
or deep-copied implicitly: its hook explicitly chooses either policy.

The synchronous [selective copy](postgres-results.md#selective-copies) uses the
same lifecycle machinery. `result.copy({.observers = false})` retains no observer
registration or instance data, invokes no copy hook and requires no later destroy
hook. Other selections populate their chosen payload before invoking observers;
a rejected copy registration is still suppressed without failing the value copy.
The source's active-dispatch contract remains in force even when observers are
disabled. An observer-free copy of a retained result can release the final callback
owner when its observed source is destroyed; it does not borrow that owner.

`close()` and `finish()` end transport activity without destroying the logical
Connection, so they do not emit `connection_destroy`. Reset retains the same
registrations and emits `connection_reset` only on success. Replacing its old
transport is not logical connection destruction. Reset does not replay old
queries, subscriptions or result creation.

## Failure and State Access

Handlers return `Result<void>`:

- Registration failure returns the handler's error, discards the registration
  and releases its owned state without a connection-destroy event.
- Result-create/copy failure suppresses that registration on the destination
  result. It receives no later copy/destroy events from that result. Other
  registrations and the query/copy operation still proceed.
- Reset/destroy return errors are ignored. They do not veto a successful reset
  or interrupt required cleanup; owning data still releases normally.

Both Connection and ResultSet expose synchronous `event_data(id)` and
`set_event_data(id, data)`. An unknown ID fails with `invalid_argument`; an
existing empty slot returns a successful empty pointer. Inside a callback,
replace its slot directly through `event.data`. External connection mutation
and registration reject deferred/active session borrows or callback invocation
with `Error::busy`; result mutation rejects its own active lifecycle dispatch.
The blocking facade forwards these synchronous APIs without driving Context.

## Execution and Limits

Session events run inline on the executing session graph; registration runs on
its caller. Result copies/destruction run on the thread performing that action,
which can be outside a Context. The callable is serialized per registration,
including results on different threads. Distinct registrations are not mutually
serialized. Keep callbacks short: they must not wait for work whose cleanup
needs the same observer.

Callback arguments, names, object pointers and source-data references are borrowed
only through invocation. Copy owning data intentionally retained afterwards.
Do not drive nested Context/Runtime loops, reenter session operations or
copy/destroy observed values from a lifecycle callback. Nested lifecycle dispatch
fails its contract before acquiring another observer lock, rather than risking
recursive destruction or lock cycles. Connection and mutable ResultSet access
remain application-coordinated; observers do not make those objects thread-safe.
Concurrent copies of an otherwise immutable ResultSet are supported.

`ResultSet` now has custom special members and is no longer an aggregate. Default
construct an application-built result and populate its fields; such results
start unobserved. Rebuild consumers for the changed C++ layout. Result wire/data
bounds remain in force, but application-owned observer state is not charged to
the protocol result budget. Ordinary allocation failure follows the existing
exception-disabled library policy, not a recoverable hook veto.

Hooks apply to actual ResultSet values, including query, batch, prepared/describe,
COPY completion, mixed Exchange and pipeline chunks. Raw Row, CopyFormat,
notifications and Diagnostic values are different API objects, not synthetic
PGresult objects. SQL errors remain Task/Result failures or `Outcome::error`;
no error ResultSet is fabricated just to emit a creation event.

## Mapping and Qualification

### Application-Built Results

The API exposes synchronous
`Connection::attach_events(ResultSet &)` and the same BlockingConnection method.
Permanent Windows/Linux Debug, Release, ASan and reduced-module controls pass,
including real PostgreSQL plain/mTLS, four-worker schedulers and relocated
component packaging. [Qualification and limits](postgres-qualification.md#application-result-attachment-qualification-2026-10-09).

Populate an owning result before attaching the Connection's current observers:

```cpp
weave::pg::ResultSet cached;
cached.kind = weave::pg::ResultKind::tuples;
cached.columns.push_back({.name = "answer", .type = 23});
cached.rows.push_back({{std::string{"42"}, weave::pg::Format::text}});
cached.command = "SELECT 1";

if (auto attached = database.attach_events(cached); !attached)
  co_await weave::fail(attached.error());
```

Attachment executes no SQL, sends no network traffic and does not drive Context.
It preserves application-populated payload/kind/metadata and invokes result-create
hooks with a live borrowed Connection and its current instance-data slot.
The payload pointer is read-only; the callback can replace its own `event.data`.
The result retains accepted owning registrations, not a Connection pointer or
implicit ownership of its instance-data registry. It can subsequently outlive
the Connection and participate in ordinary/selective copies and destruction.

Accepted EventIds are skipped on later calls, including registrations with empty
data. A rejected registration releases its new data and remains absent. All
remaining registrations are attempted, and attachment returns the first failed
Result, without rolling back accepted observers. Repeating the explicit call can
retry rejected registrations; it can also add newly registered observers or
observers from another Connection. Existing slots are not replaced or refreshed.
A failed Result remains a failure even if its contained error code is zero.

Attachment rejects deferred/active session borrows, pipeline leases, active
Connection observers, an actively dispatched result or a nested lifecycle
callback with `Error::busy`, before invoking another receiver. Logical closed
Connections can still provide their retained observer registry; moved-from
Connections retain the existing fatal-use contract. This does not make mutable
Connection/ResultSet access thread-safe or make application payload allocation
recoverable on out-of-memory failure.

Unlike libpq's separate make-empty/fire steps, the observer selection occurs at
each explicit attachment call, after payload population. No two-stage pending
registry is retained. Rejected observers cannot be retried after that Connection
is destroyed unless another live registry is explicitly supplied. Arbitrary
native error-status PGresults are not fabricated: application errors/diagnostics
remain owning Outcome/Diagnostic values with their existing lifecycle model.

### Native Mapping

[libpq's event system](https://www.postgresql.org/docs/18/libpq-events.html)
provides registration, connection reset/destruction and result creation/copy/
destruction callbacks with separate connection/result instance state. This API
maps those lifetimes to owning C++ values rather than function-pointer keys,
opaque unmanaged pointers or explicit memory-release functions. PostgreSQL 18
reset callback errors do not veto reset. This is not literal C API equivalence.

Permanent regressions cover rejection, owning state, deep copies, assignments,
reset failure, retained results, concurrent copies, duplex COPY and both
four-worker schedulers, including Windows shared IOCP. Actual disposable
PostgreSQL 18 controls cover SCRAM and verified mTLS, prepared descriptions,
multiple/empty queries, pipeline row chunks, mixed Exchange/COPY, reset and
blocking operation. The final matrix passes 198 CTest executions and three
additional native Windows real-server controls. This is scoped correctness and
packaging evidence, not an exhaustive race/security audit or complete libpq parity.
