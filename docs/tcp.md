# TCP Servers

Include `<weave/tcp/serve.hpp>` (also included by `<weave/tcp.hpp>`) and link
`weave::tcp`. No Runtime dependency is added.

```cpp
co_await weave::tcp::serve(
  "127.0.0.1",
  port,
  {.backlog = 512, .no_delay = true},
  echo);
```

The handler takes ownership of a `TcpStream` and returns `Task<void>`. A named
function works directly; lambdas, including move-only coroutine lambdas, also work:

```cpp
co_await weave::tcp::serve(
  "127.0.0.1", port, {.no_delay = true},
  [](weave::TcpStream client) -> weave::Task<void> {
    co_await echo(std::move(client));
  },
  [](std::error_code error) noexcept {
    WEAVE_LOG_ERROR("Client: %s", error.message().c_str());
  });
```

`ServeOptions` defaults to backlog `512`, `no_delay = false`, and `ipv6_only = true`. TCP_NODELAY is
configured before dispatch; configuration failures close the accepted stream and
fail the server just like `accept()` failures. Startup is lazy and resolves the
executing Context, like contextless `listen()`. The endpoint string is borrowed
until listener setup finishes.

Numeric IPv6 and owned `Endpoint` overloads are supported. See
[addresses and DNS](addresses.md) for dual-stack listening and endpoint queries.

## Buffered Data Handlers

Include `<weave/tcp/on_data.hpp>` as well, or use `<weave/tcp.hpp>`:

```cpp
static weave::Task<void> echo_data(weave::TcpStream &client, std::span<const std::byte> data)
{
  co_await client.write_all(data);
}

// Inside a Task:
co_await weave::tcp::serve(
  "127.0.0.1", port, {.backlog = 512, .no_delay = true}, weave::tcp::on_data(echo_data));
```

`on_data` is an owning callback adapter, not a new socket type or execution model.
It runs the receive loop for each client, using a reusable 4096-byte buffer in that
client's coroutine frame. `on_data<16384>(echo_data)` selects another nonzero buffer
size at compile time. It does not allocate a fresh receive buffer per read, although
coroutine frames still use the normal Task allocator.

The callback takes `TcpStream &` and `std::span<const std::byte>` and returns
`Task<void>`. It is awaited completely before the next read. Reads and callbacks are
serial per connection, but callbacks for different clients may overlap. Slow writes
therefore delay further reads from that client without blocking other clients.
EOF ends the connection's loop; the callback is never invoked with an empty slice.
Read or callback errors follow `serve`'s normal client error isolation/observer path.

The stream and slice are borrowed until the callback's Task completes, including
its cancellation cleanup. Do not retain either in independent spawned/detached work;
copy any bytes that must survive a callback. Do not move the borrowed stream out of
the callback; use an owned stream handler for socket ownership transfers.
There is no consume/retain operation, automatic message framing, growable input
queue, or read-ahead. A chunk can be only
part of a message or contain several messages. Use an owned stream handler for
custom buffering, framing, read scheduling, or connection lifetime control.

The server retains the adapter and its callback object until all client tasks drain,
including move-only coroutine-lambda captures. One callback object is shared across
clients, so shared mutable state still needs synchronization on a work-stealing
Runtime. Cancellation drains pending reads and callbacks before reclaiming their
receive buffers. Independently spawned callback work does not join automatically.

Pass the adapter by value to `serve`. For direct invocation, keep a named adapter
alive and unmoved until the returned Task finishes. Calling a temporary adapter
directly is rejected because its coroutine would borrow a destroyed callback object.

The minimal [Context](../modules/tcp/examples/echo/minimal/context.cpp) and
[Runtime](../modules/tcp/examples/echo/minimal/runtime.cpp) examples use `on_data`.
The [stream Context](../modules/tcp/examples/echo/stream/context.cpp) and
[stream Runtime](../modules/tcp/examples/echo/stream/runtime.cpp) examples
use explicit accept and receive loops, their own buffers, and `weave::detach`.
These detached clients belong to the Context/Runtime, not the accept-loop Task;
they are cancelled and drained when the Context/Runtime shuts down. The minimal
examples instead use `serve` to drain clients before the server Task returns.

## Ownership And Execution

`serve` accepts continuously and schedules each client independently through
`TaskScope`. Standalone Context drivers and worker-affine runtimes keep clients
affine; work-stealing runtimes may distribute them across workers. Sockets retain
their original I/O binding. No threads or runtime are created by `serve`.

Only dispatched handler tasks and their directly awaited work belong to this
server lifetime. Independent `spawn`/`detach` calls inside a handler do not become
server children automatically; use a nested scope when that work must also drain.

The server owns one handler object and one error observer until all client tasks
and their frames have been destroyed. They are shared, not copied per connection.
On a work-stealing Runtime, invocations and suspended handler tasks can overlap
on different workers: shared mutable captures must be synchronized. Borrowed
references still need to outlive `serve`; ownership of the callable does not make
its references owning. Client streams are passed by value.

Handler errors are recovered at the client boundary; they do not cancel sibling
clients or fail the accept loop. The optional `noexcept` observer runs on the
client's execution thread after the handler frame has been reclaimed. With no
observer, client errors are deliberately discarded. A client cancelled before
handler invocation does not invoke the handler or this observer.

Listen, accept, socket-configuration and submission errors fail the server. Before
propagating an accept/submission error, `serve` cancels and drains all its clients.
The server error takes precedence over client cancellation during cleanup.

Cancellation of the enclosing task, its join handle, or its Context/Runtime stops
acceptance and requests cancellation of client tasks. `serve` waits for their
native I/O completions and destroys their frames before returning
`operation_canceled`. Cancellation races follow the usual operation semantics;
successful work is not undone. Uncooperative handlers can delay draining. This is
cancel-and-drain, not a graceful deadline-based shutdown or a delivery guarantee.

There is currently no connection limit or overload policy. This helper does not
turn the echo examples into production servers.

## An Existing Listener

When the application needs the assigned port or custom listener setup:

```cpp
auto listener = co_await weave::tcp::listen("127.0.0.1", 0, 512);
WEAVE_LOG_INFO("Listening on port %u", static_cast<unsigned>(listener.local_port()));
co_await weave::tcp::serve(listener, {.no_delay = true}, echo, log_client_error);
```

This overload takes `AcceptOptions`, because backlog was already set by `listen`.
It borrows the listener until completion and does not close it; accepted clients
are still owned and drained. The endpoint overload owns and closes its listener.
The listener must not be moved or used for a second concurrent accept loop.

Keep using `listen`, `accept`, `spawn` and `detach` directly for custom admission,
dispatch or lifetime policies. `serve` is a convenience layer, not their replacement.
