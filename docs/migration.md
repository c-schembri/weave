# Task API Migration

The experimental Task implementation is now the library's sole coroutine model,
under the `weave` namespace, with feature-specific CMake targets. This is a breaking
change; there are no legacy aliases, bridging coroutines, or parallel library.

| Previous API | Current API |
|---|---|
| `Async<Result<T>>` | `Task<T>` |
| `Async<T>` for infallible work | `Task<T>` (all Tasks can fail) |
| `co_await client.read(buffer)` returns `Result<size_t>` | Yields `size_t` or propagates the error |
| Explicit checks at every await | Only recovery sites use `co_await as_result(operation)` |
| `co_return Result<void>{}` | `co_return` |
| Explicit void error return | `co_await fail(error)` |
| `ctx.block_on(task)` | `ctx.run(task)`, returning `Result<T>` |
| `Context ctx; ctx.status()` | `auto ctx = Context::create(); if (!ctx) ...` |
| `Runtime runtime(options); runtime.status()` | `auto runtime = Runtime::create(options); if (!runtime) ...` |
| Spawn a root and synchronously get its handle | `runtime->run(task_or_factory)`, returning `Result<T>` |
| `JoinHandle<Result<T>>` | `JoinHandle<T>` |
| `std::move(handle).get()` returns `T` | Returns `Result<T>`, including void |
| Prototype namespace/header | `weave`, `<weave/tcp.hpp>` or `<weave/task.hpp>` |

Non-void Tasks can originate failure with `co_return std::unexpected(error)`.
Synchronous `Result<T>` values are not awaitable. Check them explicitly and
propagate errors with `co_await fail(result.error())`. This applies to `spawn`,
explicit-Context `tcp::listen`, `no_delay`, `shutdown_send`, `cancel`, and `close`.
Asynchronous socket operations and runtime roots are native
Tasks, not wrappers over the former implementation.

Code following a failed participating await does not run. Use `as_result` when
you need to recover, log, or perform asynchronous cleanup before propagating.
Ordinary local destructors run when the failed chain is reclaimed. This is not
exception unwinding; exception-sensitive scope guards do not see an exception.

`when_all` waits for every child and then propagates the first error in argument
order. It does not cancel siblings. Cancellation remains a request: await/join
pending operations before releasing their buffers or sockets. Runtime spawning,
socket affinity, and scheduler choices are otherwise unchanged.

## Component split

The monolithic `weave::weave` target and `<weave/weave.hpp>` are removed.
Use `weave::core`, `weave::io`, `weave::runtime`, and/or `weave::tcp`.
Core is header-only; required lower-level dependencies propagate automatically.
Runtime does not include TCP, so networking runtime applications must include
`<weave/tcp.hpp>` and link both targets explicitly.

| Previous API | Component API |
| --- | --- |
| `ctx.listen(ip, port)` | `weave::tcp::listen(ctx, ip, port)` |
| `ctx.connect(ip, port)` | `weave::tcp::connect(ctx, ip, port)` |
| `<weave/weave.hpp>` | `<weave/tcp.hpp>` for networking, `<weave/io.hpp>` for Context only |
| Transitive Windows headers | Include native headers explicitly in native application code |
| `Context::status()` reports Winsock setup | TCP setup reports Winsock errors; `Context::create()` reports IOCP setup |

Create a Context with `auto ctx = weave::Context::create(options)` and check the
result before use. Call `ctx->run(...)` and pass `*ctx` to APIs taking `Context &`.
The result owns an immovable Context; keep it alive until all borrowing tasks,
streams, and listeners have been destroyed. The default constructor and separate
`Context::status()` query are removed. Runtime follows the same pattern with
`auto runtime = weave::Runtime::create(options)`, `runtime->`, and `*runtime`.
The unchecked Runtime constructor and `Runtime::status()` are also removed.
`Runtime::run(task_or_factory)` waits for its root without closing submissions;
destruction cooperatively cancels and drains remaining work.

Creating a Context does not initialize Winsock for unrelated native sockets.
Native application code must manage its own Winsock startup/cleanup references.

Inside a running task, prefer `co_await weave::tcp::listen(ip, port)` and
`co_await weave::tcp::connect(ip, port)` when no explicit binding is needed.
These lazy Tasks resolve the executing Context at startup, so runtime roots
can use `runtime->run(serve(port))` without a Context-taking factory.
Starting contextless setup without an active Context is a fatal contract
violation. Keep endpoint strings alive through setup; sockets keep their
selected Context even if their task later migrates. Explicit overloads remain
supported, and `tcp::listen(ctx, ...)` still returns a synchronous Result.

Inside tasks, `weave::detach(task_or_factory, on_error)` also inherits execution:
local Context ownership for standalone/custom drivers and affine workers,
independent stealable runtime ownership for work-stealing workers. Context and
Runtime parameters are unnecessary in an echo accept loop. Explicit member APIs
remain for targeting an executor or submitting outside execution. Free detach
requires an active scope and does not join children to their parent. Asynchronous
TCP now rejects incompatible Context execution before submission, even when
unrelated Contexts share a thread; synchronous setup and cleanup are unchanged.

Fresh CMake configurations build libraries only. Development presets still build
tests/examples/benchmarks explicitly. Select components with
`-DWEAVE_MODULES="tcp;runtime"`; installed consumers use
`find_package(weave CONFIG REQUIRED COMPONENTS tcp runtime)`.

Benchmark artifacts under `benchmarks/results/` are local and ignored by Git.
Historical labels and hashes describe the code measured at the time, not current
API names. The current [performance gate](gate.md) accepts an external baseline
artifact so deleting old source does not remove version-to-version regression checks.
