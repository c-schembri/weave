# Networking

## TCP

Include `<weave/tcp.hpp>` and link `weave::tcp`. Runtime is optional.
Inside a Task, setup resolves its executing Context when the lazy Task runs:

```cpp
auto client = co_await weave::tcp::connect("localhost", 8080);
auto listener = co_await weave::tcp::listen("127.0.0.1", 8080);
auto accepted = co_await listener.accept({.no_delay = true});
```

Explicit-Context overloads select a known destination. Explicit listener setup
is synchronous and returns `Result<TcpListener>`; check it normally, without
`co_await`. Contextless setup without an executing Context is a fatal contract
violation. A connected socket keeps its original binding through task migration.

| Stream operation | Kind | Contract |
| --- | --- | --- |
| `read(buffer)` | `Task<size_t>` | Partial reads; zero for EOF with a nonempty buffer |
| `read_exactly(buffer)` | `Task<void>` | Fills the span or fails on premature EOF |
| `write_all(buffer)` | `Task<void>` | Sends the entire span; source stays alive through completion |
| `shutdown_send()` | `Result<void>` | Native TCP half-close; no asynchronous shutdown exchange |
| `no_delay`, `keep_alive`, `user_timeout` | Synchronous Results | Native socket-option failures remain observable |
| `close`, `cancel` | Synchronous Results | Close rejects active I/O; cancellation does not join |

TCP carries bytes, not messages. One reader and one writer may overlap; competing
operations in the same direction fail with `busy`. Streams/listeners move only
while idle. Drain operations before moving/destructing sockets or their Context.
Native submission/completion errors propagate through Tasks; cancellation may
have partial effects. Empty buffers still require compatible execution, and an
empty read is not an EOF observation.

`local_endpoint()`, `peer_endpoint()` and listener `local_port()` are synchronous
inspection. Where their signatures return Result, native inspection can fail.
Port zero requests an available port. IPv6 defaults to v6-only; dual stack is
explicit. Backlog is a request subject to native limits, not a connection guarantee.

## Servers

For library-managed receive buffers:

```cpp
co_await weave::tcp::serve(
  "127.0.0.1",
  8080,
  {.backlog = 512, .no_delay = true},
  weave::tcp::on_data(
    [](weave::TcpStream &client, std::span<const std::byte> data) {
      return client.write_all(data);
    }));
```

For explicit stream control, pass a handler taking an owned `TcpStream` and
returning `Task<void>`. `serve` owns clients in a TaskScope: server failure or
cancellation cancels and drains them before returning. An optional `noexcept`
observer isolates client errors; accept/submission failures still fail the server.

`on_data` reuses one receive buffer per client and awaits its callback before
reading again. Data is borrowed only through that await; EOF does not invoke it.
There is no framing, read-ahead or detached callback work. Keep an adapter alive
when invoking it directly; do not invoke a temporary adapter's borrowing coroutine.

An explicit accept loop with `detach` gives independent client lifetimes instead.
Keep each client owned by its coroutine and choose an error handler deliberately.
[Self-contained echo variants and comparisons](examples/echo/README.md).

## Addresses And DNS

`IpAddress::parse` and `Endpoint::parse(literal, port)` are synchronous Results;
`to_string()` returns owning text.
Addresses own IPv4/IPv6 bytes and IPv6 scope IDs; endpoint ports are host-order.
Numeric parsing does not perform DNS. Bracket IPv6 authorities where a port is
part of the text. IPv4-mapped IPv6 remains an IPv6 address.

`resolve(host, port)` is asynchronous and returns owning endpoints. TCP connect
can consume a hostname or endpoint list; failures and cancellation are explicit.
Weave follows the system resolver rather than implementing its own DNS protocol.
Linux uses glibc `getaddrinfo_a`; native DNS/provider work may not be immediately
interruptible, so timeout/cancellation still waits for safe callback cleanup.
An already running lookup may delay a deadline. No public DNS service is required
by correctness tests.

Use `weave::parse_port(text)` from `<weave/port.hpp>` for strict numeric ports
0-65535. It returns a standard `std::expected` and does not require networking.

## Local Sockets

Include `<weave/local.hpp>` and link `weave::local`:

```cpp
auto listener = co_await weave::local::listen(path);
auto client = co_await weave::local::connect(path);
```

Local streams use the same Task, read/write, cancellation and lifetime model.
Filesystem paths are owned through lazy setup. Weave never unlinks an existing
path to make bind succeed; the application owns permissions and path cleanup.
Linux also supports abstract addresses and `peer_credentials()` from SO_PEERCRED.
Credential snapshots identify a peer, not application authorization. Windows
abstract sockets are unsupported; unsupported security capabilities return an
error rather than invented identity values.

## Generic Streams

`ReadStream`, `WriteStream`, `DuplexStream` and `CancellableStream` describe compatible
transports without a virtual base class. Include `<weave/stream.hpp>` and link Core
for concepts/helpers; actual transports supply their own module dependencies.
`stream::read_exactly` and `stream::copy` preserve borrowed-buffer
lifetimes and propagate failures. TLS adapters use these contracts; see [TLS](../tls/README.md).

## Backends

Windows uses IOCP; Linux uses liburing/io_uring. No epoll or macOS backend exists.
Each Linux Context owns its SQ/CQ and queues migrated submissions/cancellation
back to the owner. Operations remain alive until both native and cancellation
completions drain. Shared Linux I/O layout is unsupported.
Linux uses SO_REUSEADDR for listener restarts, not SO_REUSEPORT. Native error
codes can differ by platform. [Requirements and installation](../../docs/build.md).
