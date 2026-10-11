# Architecture

## Modules

```text
modules/<feature>/
  include/weave/       supported public headers
  include/weave/<feature>/detail/
                       installed template implementation, not supported API
  src/                 private compiled implementation and private headers
  src/windows/         Windows platform implementation
  src/linux/           Linux platform implementation
  tests/               feature correctness tests
  examples/            self-contained usage examples
  benchmarks/          feature-owned measurements
tests/integration/     cross-module correctness
test_support/          shared, noninstalled fixtures
benchmarks/support/    shared measurement harness
benchmarks/integration/
                       whole-system workloads
docs/                  user guides, testing and measurement contracts
```

Only create protocol modules when implementing them. Future HTTP/WebSocket
adapters should be separate where possible; using WebSocket framing should not
force a full HTTP stack on every consumer.

```text
core <- io <- runtime
          <- tcp <- tls <- postgres
          <- local       <- postgres (private transport)
          <- sync <- tls
```

PostgreSQL uses public transports, not IOCP/io_uring or scheduler internals.
Its private protocol parsers, OpenSSL Crypto and ICU do not become public native
types. libpq exists only in optional comparison targets. [Build components](build.md).

## Visibility

Public concepts live under `weave`, with feature APIs such as `tcp`, `tls` and
`pg`. Shared internal contracts belong in the owning `detail` namespace.
File/platform-local helpers belong in an anonymous namespace inside the library
namespace, before public/member definitions. Private does not automatically mean
shared `detail`.

Examples of genuinely shared machinery: IO access, operation records, execution
scope/continuation routing and frame ownership. PostgreSQL's private `PoolAccess`
is a named friend because pool.cpp must inspect/recycle Connection's private
session state implemented in connection.cpp; it is not a public customization hook.
`ContextAccess` checks execution compatibility without making protocol modules
depend on native backend headers.

## Execution And Completion

Context owns native I/O and is driven on its creating thread. Runtime coordinates
Contexts and roots; scheduling and I/O layout are independent. Windows uses IOCP,
with optional shared completion collection. Linux gives each Context one io_uring
and routes migrated submissions to that ring's owner.

Operation records and borrowed buffers survive native completion and cancellation
draining. Completion resumes through the captured Context/executor; it does not
run user code on a cancelling thread. Movable runtime roots remain serialized;
pinned roots never resume on an arbitrary collector.

Task failure is routed while the parent is suspended, before its normal
`await_resume` executes. A failed child does not destroy a live parent frame.
Ownership reclaims failed chains inside-out; recovery adapters stop propagation
at explicit boundaries. Joined children drain before enclosing frames are freed.
This is a cooperating coroutine model, not automatic error handling for arbitrary
C++ awaiters. [Public execution and lifetime contracts](execution.md).

## PostgreSQL Pooling

A pool owns immutable reconnect configuration, a bounded semaphore and stable
session entries. Acquisitions/leases retain shared pool state, not a pointer to
a potentially destroyed Pool handle. Return is synchronous; the next checkout
performs asynchronous rollback/reset before exposing the session to a new borrower.

Connection objects are not shared concurrently between borrowers. A pool belongs
to its creating Context or compatible work-stealing domain; affine workers use
separate pools. Closing wakes acquisition waiters and retires idle sessions;
existing leases drain independently and discard on return.
[Public pooling API](postgres.md#pooling).

## Scope

Implemented: core Tasks, IO, runtime, TCP, local sockets, synchronization, TLS and
native PostgreSQL. HTTP, WebSocket, portable asynchronous files, general blocking
work offload and a macOS backend are not implemented. Linux shared I/O and Linux
correctness CI remain separate work. TLS/PostgreSQL are experimental, not certified
production-ready. [Qualification and deployment limits](testing.md).
