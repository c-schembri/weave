# Weave development rules

- Windows uses IOCP; Linux uses liburing/io_uring. No epoll or macOS backend.
  Keep Linux SQ/CQ access on each Context's owner, route migrated submissions
  through its queue, and retain cancelled operations until both native and
  cancellation CQEs drain. Linux shared I/O layout is explicitly unsupported.
- C++23, CMake, exceptions disabled. Public asynchronous type is Task<T>.
- Task<T> means asynchronous T or std::error_code; participating awaits propagate
  errors automatically. Use as_result() for recovery and Result<T> at synchronous
  boundaries. Never introduce throwing operational APIs or a legacy Async layer.
- Synchronous operations are never co_awaited; Result<T> is not awaitable.
  Check results from spawn, explicit-Context listen, no_delay, shutdown_send, cancel,
  and close directly. Inside Tasks propagate synchronous failures with co_await fail(error),
  a dedicated failure-routing awaiter, not an asynchronous operation or Result adapter.
  Await actual Tasks, join handles, and asynchronous operations instead.
- Simple API ergonomics are the primary product goal; correctness is non-negotiable.
  Keep common workflows obvious, preserve clear ownership, and base performance
  decisions on measurements rather than assumptions.
- Use Context::run(task) to drive the calling thread's event loop until the task
  completes, returning Result<T>. Context::spawn owns independent tasks and returns
  a JoinHandle; Context::run() serves submissions until request_stop(), then drains.
  Runtime coordinates contexts and worker threads; it is not required for concurrency.
  Prefer one clear name per operation over aliases and redundant API choices.
- Use spawn when a join handle is wanted and detach for deliberately unjoined work.
  Both retain execution ownership; detach returns void and is never co_awaited.
  Its optional noexcept error handler observes both submission rejection and task
  failure. Without a handler, those errors are deliberately discarded. Rejection
  calls the handler synchronously on the submitter, outside admission locks;
  execution errors are reported on the execution thread after frame cleanup.
  Use Task::on_error for observing task failures without changing their result.
  Never discard a temporary join handle inside detach: a fast result could then be
  destroyed on the submitting thread rather than an execution worker.
- Inside executing tasks, weave::detach inherits the execution scope: local ownership
  for standalone/custom Context drivers and worker-affine runtimes, independent
  stealable runtime ownership for work-stealing workers. It also accepts tasks,
  factories and optional noexcept handlers, returns void, and never joins its parent.
  Outside a running scope it is a fatal contract violation; use explicit Context/Runtime
  members to select a destination or submit externally. Keep the generic submission
  hook in IO; never make IO or TCP depend on Runtime or silently pin stealing children.
- Submission accepts an owning Task<T> or a factory returning Task<T> by value.
  Factories may take no arguments or Context &; prefer the Context & form if both
  are callable. Direct tasks are constructed on the caller but run on the executor;
  factories construct tasks on the executor. Keep factory closures alive through
  completion, and never imply that a Task owns an originating coroutine-lambda closure.
  Prefer direct named-coroutine calls for simple owned work. Preserve resource
  affinity and borrowed lifetimes; rejection destroys unstarted tasks on the caller.
- Keep Context usable through public APIs by external runtimes: thread-safe spawn/detach
  and request_stop, caller-owned execution, and explicit lifetime coordination.
  Share spawn/JoinHandle lifetime machinery in IO rather than duplicating it in Runtime.
  Context tasks stay on their owning thread; Runtime supplies cross-context scheduling.
- Runtime I/O layout and scheduling are independent. Preserve the sharded IOCP
  control until evidence justifies promoting shared IOCP. Shared ports belong to
  the runtime I/O domain, not one worker; native completions retain their original
  Context for accounting/lifetime and route through the task executor or the affine
  owner's queue. Never execute an affine continuation on an arbitrary collector.
  Targeted APC wakes only interrupt alertable waits; they never resume user code.
  Do not add polling timeouts, helper threads, or shared_ptr ownership per operation.
  Shared-port native completions may dispatch an idle movable root directly on
  a collector in the same runtime. Keep pinned roots on their worker, serialize
  each root, bound dispatch by the existing IO batch/fairness budgets, and defer
  submission and frame cleanup. Sharded completions remain queue-routed. Batch
  steals publish their remaining work before waking an idle thief; never hold
  two worker queue locks together or skip a busy queue lock before parking.
  Per-worker dispatch guards and shared-collector accounting must both drain
  before any worker Context is destroyed.
- Context::run(task) does not join independently spawned tasks. Join their handles
  before borrowed data dies, or make each task own its data. shutdown() and Context
  destruction cooperatively cancel and drain owned tasks; never free active frames.
- Create contexts with Context::create(options) and runtimes with Runtime::create(options),
  returning Result<Context> and Result<Runtime>. No unchecked construction or separate
  status() check. Each object and its owning result are immovable because sockets,
  tasks, and continuations borrow stable addresses; keep them alive until all borrowers
  are destroyed and pending work is drained. Failed runtime startup stops and joins
  already-started workers before returning an error; never publish a failed runtime.
- Runtime::run(task_or_factory) schedules on workers and blocks the caller until the
  root finishes, returning Result<T>. Like Context::run(task), it does not join independent
  tasks or close admission. Do not call it from a running Context; await a spawn handle
  instead. Runtime destruction cancels and drains, so examples need not call shutdown().
- Contextless tcp::listen/connect return lazy Tasks and resolve the active Context
  when executed, never when constructed. Context::run and Runtime workers establish
  that thread-local execution scope; external runtimes get it by driving Context.
  Executing without an active Context is a fatal contract violation. Keep explicit
  Context overloads for controlled binding and synchronous listener setup. A socket
  retains its original Context/IOCP binding across task migration; do not re-resolve
  it on reads, writes, or accept. Borrowed endpoint strings must survive setup.
- Validate execution compatibility before asynchronous native setup/submission, even
  for empty buffers or immediate results: the owning Context must be running here,
  or the caller must be scheduler-routed within the same work-stealing domain.
  Matching thread IDs alone is insufficient; unrelated same-thread Context I/O
  must fail its contract rather than hang. Context-owned tasks remain affine;
  cross-Context I/O requires the runtime continuation-routing hook. Synchronous
  explicit listener setup and socket cleanup still work outside execution scopes.
- Name factory results for the object, such as auto ctx = Context::create().
  Check ctx directly, use ctx->member(), and pass *ctx where Context & is required.
  Do not add ctx_result/context_result variables or unwrap them into reference aliases
  just to preserve old call sites; migrate the callers to the factory API directly.
  Apply the same rule to runtime results: auto runtime = Runtime::create(), runtime->,
  and *runtime for Runtime &. Do not hide the checked result behind reference aliases.
- Treat "data driven" as both evidence-driven engineering and data-oriented
  storage: explicit ownership, compact hot state, batched work, no speculative
  object hierarchies or per-operation shared ownership.
- Run dedicated performance benchmarks only when intentionally changing performance
  or when explicitly requested. Routine style, naming, documentation, and linkage
  changes need applicable builds and correctness tests, not timing runs or tables.
  Existing benchmark smoke tests validate correctness, not performance.
- Correctness CI configures WEAVE_BUILD_BENCHMARKS=OFF and excludes benchmark
  smokes. Do not run networking measurements on the undersized hosted Windows VM.
  Run local Weave/Asio/Tokio runtime scaling with scripts/bench_scaling.py instead.
  Request 1/2/4/8/16/32 server cores, one worker per physical core, with a fixed
  separate client-core budget. Mark unsupported counts unavailable; never silently
  oversubscribe workers or change client placement/budget to manufacture scaling.
  Benchmark processes run sequentially, outside correctness/release pipelines.
  Publish curated README results with hardware, sampling, and source provenance;
  upload complete raw evidence as GitHub release assets, not tracked JSON/logs.
  Retain timing failures and outliers. A complete failed sweep may be archived as
  explicitly labelled diagnostic evidence; never present it as a passed run,
  silently omit failed samples, or selectively retry them for favourable results.
  Label noisy comparisons inconclusive for the affected metric. P99 noise must
  not invalidate stable throughput; client saturation is not proof of server capacity.
- When benchmarking, compare against the pinned Asio baseline using the same
  workload and build. Report regressions and uncertainty. Do not claim wins from
  noisy single runs.
- After performance changes or an explicit benchmark request, show the benchmark
  table: Weave before/after, percentage change, and the matched Asio baseline.
  Include units, repetition count, and material variability. If benchmarks were
  not rerun, say so explicitly rather than presenting old results as new.
- Keep read/write buffers and OVERLAPPED records alive through completion,
  including cancellation. Never silently destroy active coroutine frames.
- Cancellation is cooperative: direct children inherit the promise token; independent
  spawn/detach requires SpawnOptions.cancel or a TaskScope. JoinHandle::cancel requests
  cancellation without waiting; dropping a handle still does not cancel work. Normalize
  task-token/context cancellation to operation_canceled and drain native completion before
  cleanup. Use standard stop-token callback synchronization, never force frame destruction.
- Keep scope/timeout cleanup shielded from parent cancellation. Retain failed body frames
  and owning callable closures until children drain. Do not claim that scope wrappers
  extend ordinary body-local lifetimes past successful co_return; explicitly join borrowers.
- Timers belong to IO, use steady-clock deadlines and nearest-deadline waits, and route
  resumptions through existing executor/affinity machinery. Do not introduce timer threads
  or periodic polling. Timeouts cancel and drain losers; they are not hard deadlines for
  uncooperative CPU work or foreign awaiters.
- Common addresses/endpoints and async DNS belong to IO, not TCP or Runtime.
  Keep public headers free of native types. Numeric input must bypass DNS;
  IPv6 listeners default to v6-only with explicit dual-stack opt-in. Resolver
  callbacks route through the existing Context/executor, retain query/results
  through completion, and synchronize cancellation before releasing storage.
  Test native resolver races through a per-query private injection boundary,
  not global backend overrides or external/public DNS dependencies in CI.
- Add tests for immediate failure, pending completion, EOF, partial I/O, and
  cancellation when changing those paths. Run Debug and Release CTest.
- Use ad hoc tests and diagnostic probes while investigating, but delete their
  temporary source files, scripts, and fixtures before committing. Never leave
  scratch tests in the repository. Retain relevant evidence only in ignored output
  directories or a curated summary.
- Add permanent unit/regression tests at the end, after the feature or fix has
  been fully confirmed and the implementation is locked in. Do not turn exploratory
  hypotheses into committed tests; final tests must cover the confirmed behavior.
- Keep API documentation honest about unsupported features and fatal contracts.
- Do not implement TLS, HTTP parsing, or cryptography from scratch.
- Do not commit build products or benchmark result artifacts. benchmarks/results/
  is ignored local output; keep curated summaries in docs and publish raw evidence
  separately when needed. Correctness tests must use synthetic fixtures, not local results.

## Automation

- Write repository automation in Python 3.11+ using the standard library. No
  scripting-shell dependency or pip packages. Use CMake presets and CTest for
  configure/build/test workflows instead of duplicating them in a task runner.
- Python is a development-tool dependency only. Discover the interpreter only
  when tests are enabled; do not require it for library-only builds or consumers.
- Launch programs with argument lists, not shell command strings or shell=True.
  Keep native Windows process ownership and metadata collection in isolated helpers.
- Preserve the gate's total five-minute deadline, suspended-start job assignment,
  descendant cleanup, evidence validation, fixed statistical protocol, and exit codes.
  Never overwrite archived benchmark evidence when testing or reanalyzing a migration.
- Use unittest and synthetic evidence for tooling tests, registered with CTest.
  These may run in correctness CI; measurements and native benchmark smokes
  remain local opt-ins, never required CI checks.

## Module boundaries

- Organize code by feature under modules/<name>/. Each module owns its
  CMakeLists.txt, include/weave/ headers, src/ implementation, tests/, examples/,
  and benchmarks/ where applicable. Do not create empty directories.
- Example and benchmark ownership follows the feature demonstrated or measured,
  not every dependency it uses. TCP comparisons against Asio/libuv/uSockets belong
  to TCP; scheduler comparisons belong to runtime. Split mixed-responsibility files.
- Group example families under examples/<scenario>/, with API variants and library
  comparisons in meaningful subdirectories. Let the directory carry repeated naming;
  keep each source self-contained and one README for the family. Preserve executable
  names and output paths during source-only reorganizations.
- Keep shared benchmark harness code in benchmarks/support/, whole-system workloads
  in benchmarks/integration/, and ignored local outputs in benchmarks/results/.
  Root CMake assembles the suite; modules declare their own benchmark sources.
- Benchmark and example dependencies stay private to their build targets. They
  must not become library dependencies. Preserve benchmark names, parameters,
  workload bodies, and executable paths during organizational changes.
- The Trantor TCP comparison and its upstream dependency explicitly enable
  exceptions to follow that library's native model. Keep this exception isolated;
  Weave and the other examples remain exception-disabled. The Trantor example uses
  only core's standard-library port header, without inheriting core's compile policy.
- Keep the dependency graph directed: core <- io <- {runtime, tcp, sync}; TLS depends
  on TCP and Sync, not Runtime. Stream concepts/helpers belong to portable Core.
  Only TLS may discover/link OpenSSL; no OpenSSL/native types in public headers.
  TCP must not
  require runtime, and runtime must not require TCP. Core has no OS dependencies.
- Sync waiters live in coroutine frames. Register cancellation before publishing
  them, arbitrate grants/delivery/close/cancel under the primitive lock, and post
  through the captured executor. Disarm callbacks outside the lock before reclamation.
  Close wakes but does not join; primitives must outlive waiters and owned permits.
- TLS uses the proven OpenSSL engine, mandatory peer/hostname verification and
  authenticated close_notify EOF. Serialize engine calls, retain write buffers
  through retries, and never hold the transport send gate while waiting for input.
  Cancellation after TLS state advances is terminal; drain before destruction.
  TLS shutdown sends network records and is asynchronous, unlike TCP half-close.
- tcp::serve is a TaskScope-backed convenience layer: client handlers take owned
  TcpStreams and return Task<void>. Isolate handler errors with an optional noexcept
  observer; accept/submission failures and cancellation cancel and drain clients
  before returning. Retain shared handler/observer objects until all clients finish,
  preserve the executing submission policy, and never replace this with detached work.
- tcp::on_data adapts asynchronous (TcpStream &, span<const byte>) handlers for
  serve. Keep one reusable receive buffer per client and await each callback before
  reusing it. EOF does not invoke the callback. Preserve borrowed slices through
  cancellation/frame cleanup; no read-ahead, implicit message framing, or detached
  callback work. Retain the shared callback object through all client tasks and
  reject direct invocation of a temporary adapter that its coroutine would borrow.
- Export one weave::<name> CMake target per module. WEAVE_MODULES selects build
  roots and their required dependencies; find_package components select imports.
  Do not reintroduce an all-features umbrella header or monolithic library target.
- Public headers must be self-contained and free of native OS headers. Put
  template implementation headers in the owning module's <name>/detail/ directory;
  those are installed but not supported public APIs. Compiled implementation
  headers stay under src/ and are never installed.
- Keep platform implementations under their owner's src/windows/ (and
  src/linux/). TCP uses IO's private backend contract; protocol modules should use
  transport APIs, not reach into IOCP internals or the runtime scheduler.
- Put cross-module correctness tests in tests/integration/ and shared C++ fixtures
  in test_support/. Neither is installed or linked into library targets.
- Tests, examples, and benchmarks are opt-in for consumers. A library-only build
  must not fetch comparison/test dependencies. Development presets opt in explicitly.
- Run the component packaging tests when changing module or build boundaries:
  isolated builds, relocated install consumers, and standalone public-header probes.
- Add future protocols only when implementing them. Keep optional integration
  adapters separate so, for example, WebSocket framing does not require a full
  HTTP client/server stack. Do not invent empty modules or premature stream hierarchies.

## C++ style

- Use the checked-in .clang-format (clang-format 23, also bundled with the current
  VS Code C++ extension). Two-space indentation, 120-column limit, pointer/reference
  markers next to the name, function opening braces on their own line, and attached
  braces for control flow, types, and namespaces. Do not sort includes automatically.
- Leave one blank line between definitions and around namespace bodies. Within a
  function, separate setup, guards, operations, and results into readable logical
  groups. Keep closely related declarations together; do not space out every line.
- Write for human readers from the first pass: no compressed one-line definitions
  or long runs of statements without logical breaks. Expand state transitions and
  callback setup into distinct blocks; formatting is not a substitute for readable
  structure. Use named predicates instead of nested conditional expressions.
- Keep the start of an if condition on the same line as if. Wrap long argument lists
  one argument per line with a two-space continuation indent, without column alignment.
- Prefer named intermediate results and predicates to deeply nested calls, long
  ternaries, or dense compound conditions. Use auto when the initializer makes the
  type clear; keep explicit types where storage width or an external API matters.
- Store literal iteration data in a descriptively named collection before a
  range-for loop. Do not embed initializer lists in the loop header.
- Preserve short-circuit evaluation when extracting conditions. Guard pointer
  dereferences, keep conditional side effects conditional, and capture OS errors
  before another call can replace them. Readability changes must not alter ordering.
- Simple one-line statement bodies may omit braces. Use braces when a control-flow
  body wraps across lines or contains nested control flow.
- In library .cpp files, put file/platform-local helpers, types, and state in an
  anonymous namespace inside namespace weave, before public/member definitions.
  Do not add redundant static there. Reserve weave::detail for shared internal
  contracts and header-required template/access machinery, not merely private code.
  Keep shared header definitions in named namespaces with their existing linkage;
  explain exceptions such as named friend hooks. Private nested Impl definitions
  stay in their enclosing namespace. Examples, tests, and benchmark-local functions
  may retain their existing static style; do not churn unrelated files.
- Define shared numeric typedefs in modules/core/include/weave/types.hpp. Prefer i8/i16/i32/i64,
  u8/u16/u32/u64, and f32/f64 for explicitly sized Weave data. Keep std::size_t for
  sizes/indices, std::uintptr_t for pointer arithmetic, and native API types such as
  DWORD, SOCKET, int, and benchmark::IterationCount at their boundaries.
- b8/b16/b32/b64 are unsigned integer-backed boolean storage types, not bitmask
  aliases. Zero means false; nonzero means true. Keep bool for predicates and existing
  bool fields; do not change layout, atomics, or return types merely to use a b alias.
- Apply formatting/readability conventions to all first-party C++ sources, headers,
  tests, examples, and benchmark helpers. Standalone Asio/libuv/uSockets examples
  and independent support helpers keep standard/native types rather than acquiring
  a Weave dependency just for numeric aliases. Do not reformat fetched dependencies,
  generated build output, or archived benchmark evidence.
