# Weave development rules

- Windows IOCP first; Linux io_uring later. No epoll or macOS backend.
- C++23, CMake, exceptions disabled. Public asynchronous type is Task<T>.
- Task<T> means asynchronous T or std::error_code; participating awaits propagate
  errors automatically. Use as_result() for recovery and Result<T> at synchronous
  boundaries. Never introduce throwing operational APIs or a legacy Async layer.
- Simple API ergonomics are the primary product goal; correctness is non-negotiable.
  Keep common workflows obvious, preserve clear ownership, and base performance
  decisions on measurements rather than assumptions.
- Use Context::run(task) to drive the calling thread's event loop until the task
  completes, returning Result<T>. Runtime::spawn schedules work and returns a handle.
  Prefer one clear name per operation over aliases and redundant API choices.
- Treat "data driven" as both evidence-driven engineering and data-oriented
  storage: explicit ownership, compact hot state, batched work, no speculative
  object hierarchies or per-operation shared ownership.
- Run dedicated performance benchmarks only when intentionally changing performance
  or when explicitly requested. Routine style, naming, documentation, and linkage
  changes need applicable builds and correctness tests, not timing runs or tables.
  Existing benchmark smoke tests validate correctness, not performance.
- Automatic CI must configure WEAVE_BUILD_BENCHMARKS=OFF and run correctness tests
  only. Do not schedule benchmark smokes or performance gates on pushes or PRs.
- When benchmarking, compare against the pinned Asio baseline using the same
  workload and build. Report regressions and uncertainty. Do not claim wins from
  noisy single runs.
- After performance changes or an explicit benchmark request, show the benchmark
  table: Weave before/after, percentage change, and the matched Asio baseline.
  Include units, repetition count, and material variability. If benchmarks were
  not rerun, say so explicitly rather than presenting old results as new.
- Keep read/write buffers and OVERLAPPED records alive through completion,
  including cancellation. Never silently destroy active coroutine frames.
- Add tests for immediate failure, pending completion, EOF, partial I/O, and
  cancellation when changing those paths. Run Debug and Release CTest.
- Keep API documentation honest about unsupported features and fatal contracts.
- Do not implement TLS, HTTP parsing, or cryptography from scratch.
- Do not commit build products. Preserve deliberate benchmark evidence only.

## Module boundaries

- Organize code by feature under modules/<name>/. Each module owns its
  CMakeLists.txt, include/weave/ headers, src/ implementation, tests/, examples/,
  and benchmarks/ where applicable. Do not create empty directories.
- Example and benchmark ownership follows the feature demonstrated or measured,
  not every dependency it uses. TCP comparisons against Asio/libuv/uSockets belong
  to TCP; scheduler comparisons belong to runtime. Split mixed-responsibility files.
- Keep shared benchmark harness code in benchmarks/support/, whole-system workloads
  in benchmarks/integration/, and historical evidence in benchmarks/results/.
  Root CMake assembles the suite; modules declare their own benchmark sources.
- Benchmark and example dependencies stay private to their build targets. They
  must not become library dependencies. Preserve benchmark names, parameters,
  workload bodies, and executable paths during organizational changes.
- Keep the dependency graph directed: core <- io <- {runtime, tcp}. TCP must not
  require runtime, and runtime must not require TCP. Core has no OS dependencies.
- Export one weave::<name> CMake target per module. WEAVE_MODULES selects build
  roots and their required dependencies; find_package components select imports.
  Do not reintroduce an all-features umbrella header or monolithic library target.
- Public headers must be self-contained and free of native OS headers. Put
  template implementation headers in the owning module's <name>/detail/ directory;
  those are installed but not supported public APIs. Compiled implementation
  headers stay under src/ and are never installed.
- Keep platform implementations under their owner's src/windows/ (and eventually
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
- Keep the start of an if condition on the same line as if. Wrap long argument lists
  one argument per line with a two-space continuation indent, without column alignment.
- Prefer named intermediate results and predicates to deeply nested calls, long
  ternaries, or dense compound conditions. Use auto when the initializer makes the
  type clear; keep explicit types where storage width or an external API matters.
- Preserve short-circuit evaluation when extracting conditions. Guard pointer
  dereferences, keep conditional side effects conditional, and capture OS errors
  before another call can replace them. Readability changes must not alter ordering.
- Simple one-line statement bodies may omit braces. Use braces when a control-flow
  body wraps across lines or contains nested control flow.
- Do not use anonymous namespaces. Mark .cpp-local free functions, function
  templates, and namespace-scope variables static, including constexpr constants.
  Types cannot be made file-local with static; use distinctive helper names or a
  named implementation namespace to avoid cross-file definition collisions. Keep
  shared header definitions in named namespaces with their existing inline/template
  linkage, and do not change public APIs or member functions to static.
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
