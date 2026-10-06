# Stream Contracts

`<weave/stream.hpp>` belongs to `weave::core`. It introduces concepts, not a base
class, virtual dispatch, transport ownership, or another runtime:

| Concept | Required operations |
| --- | --- |
| `ReadStream` | `read(span<byte>) -> Task<size_t>` |
| `WriteStream` | `write_all(span<const byte>) -> Task<void>` |
| `DuplexStream` | Both |
| `CancellableStream` | Both, plus synchronous `cancel()` and `close()` returning `Result<void>` |

Nonempty reads return up to the supplied buffer size; zero means EOF.
`write_all` succeeds only after the entire input is consumed. Buffers remain
borrowed until completion, including cancellation. Concepts check signatures;
implementations must uphold the lifetime and cancellation contracts themselves.
TCP and TLS streams satisfy these contracts, as can in-memory or framed adapters.

## Helpers

Inside a task:

```cpp
co_await weave::stream::read_exactly(source, header);

std::array<std::byte, 16384> scratch;
auto bytes = co_await weave::stream::copy(source, destination, scratch);
```

`read_exactly` loops over partial reads and fails with `connection_reset` on
premature EOF. Empty input completes without invoking the source.

`copy` forwards bytes until EOF and returns their total count. The caller owns
the reusable scratch buffer; an empty buffer is `invalid_argument`. Count overflow
is `value_too_large`. Errors propagate automatically. No framing, read-ahead,
implicit half-close, or parallel writes are introduced.

Both helpers borrow the streams and buffers. They do not extend those lifetimes
or make an affine stream safe to use from an unrelated Context. Prefer a stream's
own `read_exactly` when available; it may enforce a whole-operation read guard.

[TCP](tcp.md) / [TLS](tls.md) / [Task lifetimes](tasks.md).
