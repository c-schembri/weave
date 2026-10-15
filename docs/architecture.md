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
  README.md            main feature guide
  docs/                additional feature guides
  cmake/               feature-owned build helpers
development/
  build/               shared CMake policies
  packaging/           package configuration and consumer tests
  testing/integration/ cross-module correctness
  testing/support/     shared, noninstalled fixtures
  benchmarks/          shared harnesses, orchestration and tooling tests
docs/                  repository overview, feature matrix and build guide
```

Each development component owns its CMake registration. Feature-specific tools
and their tests stay with the module; shared infrastructure is never installed
as a public Weave component. Local build outputs and archived benchmark evidence
are not source modules.

Only create protocol modules when implementing them. Future HTTP/WebSocket
adapters should be separate where possible; using WebSocket framing should not
force a full HTTP stack on every consumer.

```text
core <- io <- runtime
          <- tcp <- tls <- {postgres, mysql}
          <- local       <- postgres (private transport)
          <- sync <- tls
core <- database <- {postgres, mysql}
```

PostgreSQL and MySQL use public transports, not IOCP/io_uring or scheduler internals.
Their private protocol parsers and authentication dependencies do not become public
native types. libpq/libmysqlclient exist only in optional comparison targets.
[Build components](build.md).

### Database Foundation

The foundation separates reusable database machinery from PostgreSQL and MySQL,
preserving their vendor-specific APIs. Additional drivers and compatibility targets
are listed in the [feature matrix](features.md#planned), not implemented modules.

- **`weave::database`:** Core-only arena/container storage, column lookup, fixed-array
  selector binding, record mapping and synchronized pool inventory/options.
  Drivers supply codecs and storage access; PostgreSQL OIDs,
  binary layouts and command statuses do not become universal database types.
- **Internal async pool support:** build-only failure-safe semaphore admission
  guards in Database's private headers, using IO/Sync without adding those
  dependencies to `weave::database`. Drivers use existing semaphore/timer operations
  and supply connect, reset, health, retirement and execution-affinity policies. A synchronous
  embedded driver need not acquire a Context or depend on networking.
- **Vendor implementations:** protocol or embedded/native engine, authentication,
  parameters, transaction/recovery semantics, cancellation and advanced features.
  Keep dependencies selective; using one driver must not link another vendor's SDK.

Common row access can look alike without erasing semantics. Retain exact values
and vendor metadata; do not force decimals through double or all fields through
text. Owning results, cursor-borrowed views and session-bound LOBs need distinct
lifetime contracts. Cancellation is a request, not proof of rollback or safe reuse.

Prefer compile-time policies over a mandatory virtual Connection or universal
owning value container. Extract proven reuse, validate boundaries against real
drivers, and keep protocol-specific tests. No ORM, SQL dialect rewriting,
automatic query replay or speculative driver directories are part of this work.

Shared mappings use `weave::db::RowTraits<T>` and `db::field`; PostgreSQL's existing
`pg::RowTraits<T>` specializations still work and override a shared mapping.
`db::decode<T>(row)` accepts an lvalue row adapter with named field selection and
`Field::as<T>()`, returning `Result<T>` without requiring a Context. Vendor APIs
remain the normal entry points, including PostgreSQL's `row.as<T>()` and cached
`result.decoder<T>()`. Shared schema lookup/binding does not own row buffers or LOBs.
The owning arena containers and driver-validation helpers live in installed `detail`
headers, not a new universal result type. Reflection stays out of primitive headers.
Pool inventory is also Core-only: a synchronous embedded pool can reuse its
bookkeeping without acquiring a Context or linking async admission machinery.
Async admission is not an installed header or a selectable package component.

## Visibility

Public concepts live under `weave`, with feature APIs such as `tcp`, `tls`, `pg`
and `mysql`. Shared internal contracts belong in the owning `detail` namespace.
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
C++ awaiters. [Public execution and lifetime contracts](../modules/io/README.md).

## Database Pooling

A pool owns immutable reconnect configuration, a bounded semaphore and stable
session entries. Acquisitions/leases retain shared pool state, not a pointer to
a potentially destroyed Pool handle. Return is synchronous; the next checkout
performs asynchronous rollback/reset before exposing the session to a new borrower.

Connection objects are not shared concurrently between borrowers. A pool belongs
to its creating Context or compatible work-stealing domain; affine workers use
separate pools. Closing wakes acquisition waiters and retires idle sessions;
existing leases drain independently and discard on return.
[PostgreSQL pooling](../modules/postgres/README.md#pooling) / [MySQL pooling and reset](../modules/mysql/README.md#blocking-and-pooling).

## Scope

Implemented: core Tasks, IO, runtime, TCP, local sockets, synchronization, TLS and
native PostgreSQL/MySQL. HTTP, WebSocket, portable asynchronous files, general blocking
work offload and a macOS backend are not implemented. Linux shared I/O and Linux
correctness CI remain separate work. TLS and database clients are experimental, not certified
production-ready. [Qualification and deployment limits](../development/testing/README.md).
