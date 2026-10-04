# Fallible Tasks: Design And Contracts

## Verdict

The requested syntax is possible with standard coroutine mechanisms, without
exceptions, macros, or a `try_` wrapper at each propagation site. It requires a
cooperating coroutine model, not just a nicer return-type alias. Weave
implements that model; it does not turn arbitrary C++ awaitables into fallible
Tasks.

`await_transform` selects the policy. It cannot inject `co_return` into the
caller. Likewise, an ordinary `await_resume()` returning `T` cannot decide to
return from its caller instead. Failure must be intercepted while the caller
is suspended, **before** its `await_resume()` executes. The language permits an
`await_suspend()` to select another coroutine handle for transfer, and only
evaluates `await_resume()` if the suspended coroutine is resumed. That is the
basis of this design. [C++ draft: await expressions](https://eel.is/c++draft/expr.await)

## Control flow and ownership

The implementation is in [task.hpp](../modules/core/include/weave/task.hpp), part of `weave::core`:

1. A move-only `Task<T>` owns a lazy coroutine frame. Its promise contains value
   storage, an error, explicit lifecycle state, a success continuation, a failure
   parent, and an active child ownership link.
2. `Promise::await_transform(Task<U>&&)` produces a `TaskAwaiter<U, false>` that
   takes ownership from the Task object. At suspension it links the promises
   and returns the child's handle. Success transfers back to the parent, whose
   awaiter extracts `U` and destroys the completed child.
3. A failing child records an error and walks the failure-parent links. Each
   skipped promise becomes failed, but none of those coroutine bodies resumes.
   The walk returns the nearest recovery boundary's continuation.
4. `as_result(task)` establishes such a boundary. Its awaiter extracts the
   `Result<U>` and reclaims the failed chain before allowing handler code to
   observe completion. `Context::run` and runtime roots provide outer boundaries.
5. Cleanup follows ownership links inside-out and disarms them before destroying
   frames. This avoids recursively invoking 20,000 nested awaiter destructors.
   No frame is destroyed by the failure-routing walk itself.

The parent has not magically executed `co_return`, nor has it reached its own
final suspension. Calling its `final_suspend()` member manually would not change
that. Explicit state is essential: `handle.done()` is not a generic indication
that this error-model computation is complete. Destruction of a suspended
coroutine is permitted and leaves its active scopes, running local destructors.
Destroying a running coroutine is not permitted. [C++ draft: coroutine definitions](https://eel.is/c++draft/dcl.fct.def.coroutine)

Success/error transfers return handles rather than recursively calling
`resume()`. Compiler code generation still needs testing; standard control-flow
semantics alone are not a performance guarantee. The tests exercise deep
immediate and delayed failures in Debug, Release, and MSVC AddressSanitizer.

## Integration boundaries

Socket operations return native `Task<T>`. The former coroutine implementation,
native-operation adapter, derived Context, and runtime bridge have been removed.
Context drives the root directly; Runtime owns the factory and root Task until
completion, then destroys the failed/successful chain and factory before
publishing `Result<T>` to a `JoinHandle<T>`.

`JoinHandle<T>::get()` returns `Result<T>`. Awaiting a handle inside a Task yields
`T` or propagates its error; `as_result(handle)` permits recovery. A lazy Task
adapter connects asynchronous joins to their owning executor. That adapter is
not used by socket operations or synchronous joins.

Awaiting a synchronous `Result<T>` uses the same policy: a value is ready; an
error suspends the Task and routes failure. `co_await fail(ec)` is just an error
Result. Non-void Tasks also accept `co_return std::unexpected(ec)`. A void promise
uses `return_void`, so its explicit error-origin syntax is `co_await fail(ec)`
rather than trying to combine `return_void` and `return_value` in one promise.

Recovery is explicit, propagation is implicit:

```cpp
auto result = co_await weave::as_result(client.read(buffer));
if (!result) {
  // Recover, translate, or report the error here.
  co_return;
}
```

Foreign awaiters are not automatically error-aware. A foreign coroutine can
consume a Task through `as_result`, not raw `co_await Task<T>`.
An explicit `try_` adapter would still need comparable completion/ownership
machinery; changing syntax alone does not solve the hard parts.

## Risks that the syntax hides

- **Outstanding I/O:** suspension is not permission to free a buffer that IOCP
  still owns. A cancellation request must be followed by completion draining.
  The native I/O Task routes failure only after submission failure or completion
  and retains the operation record until cleanup.
- **Concurrent children:** a failed child cannot simply destroy its parent while
  siblings are running. `when_all` joins every sibling and then propagates an
  error. It does not implement fail-fast cancellation. An unending sibling means
  an unending join unless explicitly cancelled.
- **Lifetime publication:** continuation handles and completion callbacks must
  never outlive their owners. No code may access an awaiter after transferring
  control to something that can destroy it. Active Task destruction is a fatal
  contract, not implicit cancellation.
- **Destruction versus unwinding:** ordinary RAII works, but there is no exception
  in flight. Exception-sensitive scope guards cannot detect this as failure;
  statements after a failed await, including asynchronous cleanup, do not run.
  Async scope-exit support would require a separate design.
- **Scheduling:** the runtime serializes execution within each root. The model
  is tested under both schedulers but is not an independently
  thread-safe promise graph. Migrating a running coroutine, sharing Tasks, and
  arbitrary concurrent completion publication are not added here.
- **Resource failure:** frame-allocation failure and violated contracts terminate
  rather than reporting operational errors. Nothrow value movement is required. This is not a claim that
  every possible program failure becomes `std::error_code`.

## Existing precedents

Folly has `co_nothrow`, promise-level await transformation, and error-aware
continuation handles that can bypass throwing during propagation. Its payload
and default Task behavior are exception-oriented, so it is a precedent for the
control-flow technique, not the proposed error-code API or a drop-in no-exceptions
dependency. [Folly Nothrow](https://github.com/facebook/folly/blob/main/folly/coro/Nothrow.h),
[BasePromise](https://github.com/facebook/folly/blob/main/folly/coro/BasePromise.h),
[Task](https://github.com/facebook/folly/blob/main/folly/coro/Task.h)

libunifex has a distinct `unhandled_done` continuation for propagating a stopped
operation through coroutine callers. Its ordinary error path is different and
can rethrow exceptions. The stopped path demonstrates the related technique of
not resuming the normal body. [libunifex Task implementation](https://github.com/facebookexperimental/libunifex/blob/main/include/unifex/task.hpp)

## Adoption

Task is now the sole public coroutine model, selected for its ergonomics. This
is an intentional breaking API change, not a claim that every performance bound
was established: the final prototype gate retained two unresolved p99 checks.
See the [historical prototype report](../benchmarks/results/2026-10-04-paired-ci-gate/README.md)
and [concurrent validation](concurrent.md). The
[native promotion run](../benchmarks/results/2026-10-04-task-promotion/README.md)
passed correctness and sanitizer tests, but failed six same-code performance
controls and flagged two historical p99 regressions. It is not a parity pass.
Only Windows/MSVC is validated;
Linux/io_uring and other compilers remain untested. Promotion does not change the
pending-I/O, cancellation, or ownership contracts above.
