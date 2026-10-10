# PostgreSQL Tracing

Tracing is disabled by default. Install an owning, move-only `noexcept` observer
on an idle connection; registration is synchronous:

```cpp
auto previous = connection.on_trace({
  .handler = [](const weave::pg::TraceMessage &message) noexcept {
    WEAVE_LOG_INFO("PG %c type=%u length=%u",
      message.direction == weave::pg::TraceDirection::frontend ? 'F' : 'B',
      static_cast<unsigned>(static_cast<unsigned char>(message.kind)),
      message.length);
  }
});
if (!previous)
  co_await weave::fail(previous.error());

co_await connection.query("SELECT 1");

auto restored = connection.on_trace(std::move(*previous));
if (!restored)
  co_await weave::fail(restored.error());
```

`on_trace({})` disables tracing and returns the previous owning configuration.
Both `Connection` and `BlockingConnection` expose the same registration API.
It rejects active or deferred session operations, pipelines and other session
leases with `Error::busy`. Install/replace observers only while the session is
idle; connection methods do not become generally thread-safe.

To observe startup, provide the additional trailing argument:

```cpp
weave::pg::Trace trace{.handler = observe_protocol};
auto connection = co_await weave::pg::connect(options, {}, std::move(trace));
```

The second argument is the existing notice handler; use `{}` when none is
needed. Diagnostic-output overloads and `BlockingConnection::connect` also
accept this trailing trace configuration. Existing connect overloads remain
unchanged. Unstarted Tasks already own their observers, but emit no events.
The receiver survives host attempts, connection moves, OAuth reconnects and
successful or failed resets. Failed replacement leaves it intact.

## Content and Security

Each `TraceMessage` reports direction, message tag and wire length. Length
includes PostgreSQL's four-byte length field but excludes its one-byte tag.
`kind == 0` denotes an untagged startup packet or SSL request. Empty tagged
messages still produce events.

Default `TraceContent::metadata` withholds every body: `payload` is a null,
empty span, `redacted` is true and `truncated` is false. Explicit application
disclosure is available:

```cpp
weave::pg::Trace trace{
  .handler = observe_protocol,
  .content = weave::pg::TraceContent::application,
  .payload_bytes = 256
};
```

Application mode delivers at most `payload_bytes` per message. The default cap
is 4096 bytes; zero is allowed, and values above 16 MiB or invalid content enums
are rejected with `invalid_argument`. `truncated` indicates an omitted
application tail; the reported wire length remains unchanged.

**Startup bodies, all pre-ReadyForQuery bodies, frontend authentication (`p`),
backend authentication (`R`) and backend cancellation keys (`K`) are always
withheld.** This also covers semantically invalid late authentication/key
messages. Invalid lengths, bodies exceeding `Limits::message_bytes` and
incomplete frames are not delivered as complete backend events. Semantic or
result-admission errors discovered after framing can still follow an event.

This is not general-purpose secret sanitization. Opted-in SQL, Bind parameters,
rows, notices, error text and COPY data can contain credentials or personal
data. The application owns log policy, storage, retention and access controls.
Do not enable application payloads indiscriminately in production.

## Execution and Scope

Observers execute synchronously on the connection's executing graph, without
helper threads. One receiver serializes its callbacks through pipeline/COPY
send-receive traffic and reconnects. No receiver lock spans an await. Different
connections and different notice/trace observers are not mutually serialized;
synchronize any shared captured state yourself.

`payload` is borrowed only during invocation. Copy deliberately retained data
inside the callback. Do not block, throw, drive a nested Context/Runtime,
replace the observer, or reenter session execution from a callback. Observing
traffic does not confer permission to mutate a connection concurrently.

Frontend events describe submitted/attempted logical frames, before the
transport write completes. They do not prove successful transmission; cancellation
or I/O failure may follow. Backend events describe fully received, bounded
frames before semantic interpretation; a subsequent protocol error is possible.
Duplex directions have no additional guaranteed total wire order.

Tracing reports logical PostgreSQL frames even over verified TLS or GSS. It
does not report encrypted records, TLS/GSS handshake records, GSS negotiation,
the unframed SSL response, OAuth HTTPS traffic or the independent CancelHandle
transport. Authentication acquisition remains private.

This maps the diagnostic purpose of [libpq tracing](https://www.postgresql.org/docs/18/libpq-control.html),
not its FILE ownership, text formatter, timestamp or regression-ID flags.
Applications can format events and attach their own timestamps. Disabled
tracing adds no observer allocation, buffer copies or wrapper coroutine.

## Tests

`weave_postgres_trace` covers redaction, truncated/metadata bodies, deferred
lifetimes, invalid configuration and late backend keys. `weave_postgres_trace_sessions`
uses owned synthetic peers for split pipelines, duplex COPY, reset failure,
cancellation, callback ownership and the blocking facade; Runtime builds also
exercise four-worker schedulers and Windows shared IOCP. The opt-in
`weave_postgres_trace_live` uses a disposable PostgreSQL 18 server for actual
SCRAM and verified mTLS SCRAM-PLUS, including four-worker workloads.

See the [qualification record](postgres-qualification.md) for the configurations
actually run. Synthetic controls are not deployment/security-audit evidence.
