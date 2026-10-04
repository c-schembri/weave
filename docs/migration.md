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
| `JoinHandle<Result<T>>` | `JoinHandle<T>` |
| `std::move(handle).get()` returns `T` | Returns `Result<T>`, including void |
| Prototype namespace/header | `weave`, `<weave/tcp.hpp>` or `<weave/task.hpp>` |

Non-void Tasks can originate failure with `co_return std::unexpected(error)`.
Tasks may also `co_await Result<T>` to unwrap a synchronous setup result or
propagate its error. Asynchronous socket operations and runtime roots are native
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
| `Context::status()` reports Winsock setup | TCP setup reports Winsock errors; Context reports IOCP setup |

Constructing a Context no longer initializes Winsock for unrelated native sockets.
Native application code must manage its own Winsock startup/cleanup references.

Fresh CMake configurations build libraries only. Development presets still build
tests/examples/benchmarks explicitly. Select components with
`-DWEAVE_MODULES="tcp;runtime"`; installed consumers use
`find_package(weave CONFIG REQUIRED COMPONENTS tcp runtime)`.

Historical benchmark evidence is retained under `benchmarks/results/`. Its old
labels and hashes describe the code measured at the time, not current API names.
The current [performance gate](gate.md) accepts a baseline artifact so deleting
old source does not remove version-to-version regression checks.
