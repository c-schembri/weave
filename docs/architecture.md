# Architecture

## Priorities

Correctness is a constraint. Within that constraint: measured throughput,
latency, memory usage, and CPU efficiency first; ergonomics second. Keep the
public API small without hiding ownership costs or inventing performance claims.

## Data and execution

Each Context owns one IOCP and a contiguous 64-entry completion buffer.
GetQueuedCompletionStatusEx dequeues a batch. A completion points directly to
the Operation embedded in its suspended coroutine frame; there is no virtual
dispatch, per-operation shared_ptr, or separately allocated callback record.

Async frames can still allocate. This is NOT a zero-allocation runtime. Frame
allocation counts and sizes need profiling before adding pools or arenas.
Likewise, the initial operation awaiter contains accept-specific scratch space
even on the receive/send paths; specialization is a candidate, not a measured win.

Operations update Context counters for submissions, completions, and dequeue
calls. These are single-thread-owned counters, not atomic shared hot state.
The OS owns the completion queue. Do not add container or object hierarchies
without a demonstrated data-access need.

## IOCP lifetime rules

Synchronous success from an overlapped call still queues completion: await it.
Only immediate non-pending errors resume inline. Never enable skip-completion
modes without changing that rule and testing both paths.

OVERLAPPED is the first member of standard-layout Operation, checked statically.
The operation and its buffer remain alive until the packet is dequeued. On
failed entries, WSAGetOverlappedResult translates the native completion status
to a Winsock error while the socket is still open.

CancelIoEx requests cancellation, but does not join it. Closing or destroying a
borrowed stream before completion is forbidden. close() returns an in-progress
error rather than releasing memory still in use by the kernel.

The Context cannot be recursively pumped. Children return by symmetric transfer;
when_all retains their frames until the last child completes. No detached work,
thread pool, cross-thread posting, or implicit global runtime exists yet.

## Platform boundary

Windows definitions currently appear in the public header. This is an explicit
first-slice compromise, not the intended Linux ABI. When adding io_uring, keep
operation/completion semantics and ownership contracts, but select concrete
backend storage at compile time rather than add per-operation virtual calls.

## Next evidence to collect

1. Allocation counts and bytes per roundtrip; coroutine frame sizes.
2. Many-connection and pipelined throughput, not only serial loopback latency.
3. Completion batch occupancy and scheduler overhead under concurrent load.
4. Tail latency with backpressure and slow readers.
5. Cancellation/close races, long-running stress, and sanitizer runs.
6. Only then compare changes such as frame pooling, specialized operation
   storage, larger batches, and completion bypass on synchronous success.

## Platform references

- [GetQueuedCompletionStatusEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getqueuedcompletionstatusex)
- [AcceptEx](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nf-mswsock-acceptex)
- [ConnectEx](https://learn.microsoft.com/en-us/windows/win32/api/mswsock/nc-mswsock-lpfn_connectex)
- [CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)
