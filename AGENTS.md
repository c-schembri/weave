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
  noisy single runs. Protocol modules use their matched client baseline instead
  (libpq for PostgreSQL), not an unrelated TCP workload.
- After performance changes or an explicit benchmark request, show the benchmark
  table: Weave before/after, percentage change, and the matched Asio baseline.
  For a new protocol without an earlier implementation, mark the before value
  unavailable and report the matched protocol baseline; never invent a before run.
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
- In the exception-free doctest configuration, failed REQUIRE assertions log a
  failure but do not terminate the test body. Add explicit control-flow guards
  before dereferencing failed Results or unavailable setup objects. Wall-clock
  expiry tests must allow capture across whole-second boundaries, then verify
  expiry without weakening the production lifetime policy.
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
  Use wsl --exec for Linux argument lists so filters are not interpreted by a
  shell. CTest qualification must use --no-tests=error; an empty selection is
  missing evidence, not a successful gate.
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
- Keep the dependency graph directed: core <- io <- {runtime, tcp, local, sync}; TLS depends
  on TCP and Sync, not Runtime. Stream concepts/helpers belong to portable Core.
  TLS owns SSL; PostgreSQL may privately link OpenSSL Crypto for authentication
  and ICU for SASLprep. No OpenSSL, ICU, libpq or native types in public headers.
  PostgreSQL implements the wire protocol using public transport APIs and never
  links libpq except in optional comparisons. Match PostgreSQL benchmarks against
  libpq, rather than an unrelated Asio TCP workload. Keep its feature matrix honest.
  TCP must not require runtime, and runtime must not require TCP. Core has no OS dependencies.
- Local streams use IO's shared private socket awaiters, retaining the same completion,
  cancellation and execution-affinity contracts as TCP. Keep filesystem paths and
  abstract names owned through lazy setup. Never unlink paths automatically or delete
  existing files to make bind succeed. Linux peer credentials come from SO_PEERCRED;
  report unsupported native security capabilities explicitly rather than fabricating IDs.
  Windows abstract sockets are currently unsupported pending native qualification;
  do not add a blocking connect fallback or helper threads.
- PostgreSQL reset requires fresh Options and must reject outstanding session
  Tasks, including deferred Tasks. Keep private borrow leases in coroutine parameters
  before initial suspension and across callbacks/chunked operations. Do not free a
  session still referenced by a frame, retain old sessions indefinitely, or add
  shared allocation/wrapper coroutines just for this guard. Parent cancellation
  stops host failover; authentication, TLS verification and protocol errors must
  never fall through to another host or downgrade transport security.
  Explicit TLS allow/prefer policies are the only transport-policy exceptions:
  allow upgrades on pre-credential SQLSTATE 28000, and prefer retries transport/TLS
  handshake failure or accepts an SSLRequest N. Pin retries to the same endpoint
  under the original deadline. Verification, revocation, malformed negotiation,
  provider errors, authentication rejection and cancellation remain terminal.
  Credential-owning lazy Tasks must acquire their cleansing owner in coroutine
  parameters before initial suspension, not only in body-local cleanup guards.
  Cover dropped/rejected unstarted connect/reset Tasks and retain move-source
  cleanup through internal handoffs without extra shared allocation or wrapper Tasks.
  Authentication-owned text, ICU work buffers and wire responses use the private
  cleansing allocator so growth and discarded capacity are cleared on release.
  Keep cryptographic intermediates in move-only cleansing arrays across Result
  returns and every failure path. Never concatenate passwords/proofs through
  ordinary string temporaries or change ordinary query storage for this policy.
  Authentication restrictions never enable weak password methods by themselves;
  SCRAM passthrough keys own fixed-size credential bytes and clear moves/destruction.
  Discard the selected session's copies after successful startup. Keys never
  bypass server-proof, channel-binding, nonce or iteration validation.
  Derive missing keys only from the selected password; reject client-only keys
  without a password before the initial SCRAM response. Keep middleware key
  options explicit, including service loading; never log them or claim libpq
  partial-key identity without testing the pinned baseline.
  cleartext still requires verified TLS or mutually authenticated GSS encryption
  and an explicit opt-in. Enforce one complete
  authentication exchange, reject disallowed challenges before credentials, and
  accept version negotiation only before authentication. Preserve protocol bounds
  and version-specific cancellation keys across reset and both connection facades.
  Cancellation Tasks are independent: capture owning backend/security snapshots
  before initial suspension, never a borrowed Connection pointer. Reset must not
  retarget a deferred cancellation Task. Blocking cancellation runs outside an
  executing Context; copied handles share immutable state, not the query stream.
- PostgreSQL password APIs use consistent user/password argument order. The free
  verifier is synchronous; connection policy lookup and changes use Task/Result
  facades. Copy passwords into cleansing owners before initial suspension and
  retain the session borrow even for dropped/rejected Tasks. Keep one operation
  lease across policy lookup and ALTER, resolve identifier encoding after policy
  responses, and construct verifier-bearing SQL directly in cleansing storage.
  Never expose typed password-change request/response bodies through application
  tracing. Automatic MD5 selection needs its own opt-in; authentication policy
  does not enable it. Hashing is synchronous CPU work, not preemptible async I/O;
  cancellation after sending a mutation does not prove it was rolled back.
- Connection/TLS metadata is synchronous and owning. Inspect actual selected
  endpoints and negotiated security, not unused options or configured intent.
  Connection snapshots require an idle serialized session, including no deferred
  Task or pipeline lease; failure must not retire a usable transport. Export
  authenticated peer certificates only, never passwords, cancellation keys,
  tickets or native handles. Copies outlive owners but are historical snapshots,
  not thread-safety, live revocation or application-authorization guarantees.
- PostgreSQL parameter inspection returns Result<optional<string>> synchronously:
  absent and reported-empty are distinct, values own their storage, and outstanding
  Tasks/leases/callbacks reject inspection without closing the transport. Numeric
  version snapshots use bounded native-compatible prefix/formula conversion;
  malformed negative/overflow inputs safely yield zero while retaining raw text.
  Effective server_options is Startup request text, not current GUCs or a complete
  option schema. Arbitrary application option text is not sanitized; never log it
  automatically or clone credential-bearing Options for metadata introspection.
- PostgreSQL option_schema is immutable static metadata, never an ambient-source
  read or a live/resolved Options snapshot. Keep keyword and ordinary environment
  recognition derived from it; retain dedicated service-file and conditional
  legacy-alias loader paths. Describe Weave defaults and value/platform limits,
  not fictitious libpq compatibility. Secret markers include SCRAM keys and key-log
  surfaces even when native descriptors call them debug fields. Do not enable an
  unsupported setting or retain credential-bearing Options just to enumerate it.
- PostgreSQL OptionsInfo owns a nonsecret configuration whitelist, not negotiated
  session state. Options::info is pure inspection; Options::defaults explicitly
  loads sources and cleanses temporary credentials. Connection::configuration is
  synchronous, serialized and readable after transport closure, but rejects outstanding
  Tasks, leases and callbacks. Never copy Options and then erase credentials for
  inspection, or retain TLS/GSS/OAuth providers in snapshots. Provider-presence flags
  do not validate providers or describe negotiated security. Loader origin records
  policy and selected paths, not per-field provenance or proof a password file was
  read. Keep configured host lists separate from actual ConnectionInfo selection.
- PostgreSQL transaction inspection is synchronous and serialized, including query
  callbacks. active means an open SQL transaction; in_progress means pending protocol
  work, and closed/moved sessions are unknown. Deferred Tasks, metadata guards and
  passive notification waits do not alone imply progress. Mark commands before trace
  callbacks; only valid ReadyForQuery resolves them, without clearing later pipeline
  command/Sync debt. Barrier snapshots retain their own server-reported state.
- PostgreSQL pipeline_status is synchronous serialized lease/recovery inspection,
  not transport health or SQL transaction state. Report off only without a lease,
  and aborted only for the observed server abort-until-Sync latch. A valid parsed
  Sync clears that latch before application consumption; terminal I/O does not
  fabricate aborted or release the lease. Preserve explicit finish synchronization
  and keep scalar inspection usable in serialized callbacks without new wrappers.
- PostgreSQL ConnectionReport is opt-in, bounded connection/reset failure history.
  Latest-attempt and monotonic phase timings update on stage transitions/completion;
  they are serialized execution-Context snapshots, not thread-safe polling or CPU
  metrics. Keep successful attempt facts without admitting unbounded extra history.
  Keep the report borrowed through Task completion, untouched for unstarted/rejected
  Tasks, and independent afterwards. Capture every failed address/host in actual
  execution order; never change retry/security policy or replace the primary error
  when history fills. Mark omitted diagnostics explicitly, preserve timeout ownership
  after cancellation drains, and never retain Options or authentication buffers.
- AuthenticationInfo owns scalar method/challenge/completion facts in session,
  report and retained-attempt snapshots. password_requested is a recognized demand,
  not proof of password transmission/use; password_missing records effective absence
  before cleanup and can be true on successful SCRAM key login. complete means
  validated AuthenticationOK, not ReadyForQuery or authorization. Keep latest report
  facts even when history fills, preserve per-attempt facts, and never fabricate
  requests from malformed/unsupported payloads or change retry/security policy.
- PostgreSQL last_failure is a synchronous owning snapshot readable after transport
  failure, but not while session Tasks/leases/callbacks are outstanding. Keep the
  primary local error separate from the retained server Diagnostic. Observe admitted
  exchanges through their existing guards, binding only the guard's own promise
  error slot at an immediate await; destroy the guard before that promise. Never
  retain a child handle/error slot after child cleanup or add wrappers to every
  transport read/write just for inspection. Preserve terminal causes through sibling
  cancellation/closed retries and keep pre-admission/application errors explicit
  in their original Task/Result rather than implying they were wire failures.
- Native GSSAPI/SSPI engines stay private; public GssContext explicitly owns bounded
  provider workers. Capture Linux's credential-cache source or Windows's effective
  caller token at factory time; never silently change identities on a worker.
  Reject inadequate impersonation levels and never fall back after capture failure.
  Provider calls can contact a KDC: never run them on an
  I/O worker or resume cancellation before native work drains. Bound admission,
  capture diagnostics on the provider thread, and retain frame/Context/executor
  lifetime through completion publication. A GSS major failure remains a failure
  even when its mapped minor describes native zero. SSPI mutual authentication
  requires a confirmed Kerberos mechanism, not just ISC_RET_MUTUAL_AUTH; local
  NTLM negotiation can report that flag without a Kerberos server proof.
  Reserve cleanup admission per session and shield native destruction from both
  task cancellation and Context shutdown. Synchronous retirement transfers an idle native
  context through its preallocated queue record; never delete it on an I/O thread
  or retain a pool owner on a provider worker. Last-owner pool teardown joins and
  drains transferred destruction before releasing identity/worker storage. All
  session borrowers must drain before explicit cleanup or retirement. Native work
  must drain, so deadlines do not promise preemption. Keep session diagnostics and
  metadata as owning, synchronized snapshots when another provider operation can
  mutate them; never return a borrowed diagnostic across that race. Non-mutual
  authentication requires verified TLS; GSS authentication alone is not GSS-encrypted
  transport. Positive Windows domain/Kerberos login needs its own qualification,
  not local NTLM evidence.
- GSS encryption is opt-in with an explicit provider. Read exactly one GSSENCRequest
  response byte; never expose unauthenticated server error text. Prefer may fall
  back only after an explicit N, using the configured TLS/plaintext policy. Never
  reuse that negotiation socket for direct TLS: close it and reconnect to the same
  selected endpoint with native socket controls reapplied. Fresh direct-TLS setup
  failures remain terminal, not permission for another host or a downgrade. Never
  retry after a proof, record, socket or framing failure. Encryption takes priority
  over TLS and requires Kerberos mutual/confidential/integrity/replay/sequence
  protection. Bound records to 16 KiB including framing; reject signed-only input.
  GSS has no authenticated close_notify; accept EOF only at record boundaries.
  Each encrypted connection retains provider admission; cancellation needs another
  slot and snapshots must require encryption even after an original prefer policy.
  Finish/reset drain native destruction before slot reuse; synchronous socket close
  retains native ownership until cleanup/destruction. Never substitute plaintext
  cancellation, destroy borrowed native contexts or run provider work on I/O threads.
- PostgreSQL connection-string parsing is synchronous and side-effect-free.
  OAuth tokens and authentication responses use the private cleansing allocator;
  validate bearer grammar and bounds before allocation. Owning async providers
  retain their callable and request in coroutine parameters before initial
  suspension, including coroutine-lambda closures. Copies may invoke the same
  const callable concurrently; const does not synchronize captured mutable state.
  Drain cancelled children before releasing provider ownership. Do not claim
  OAuth connection support from a token callback or standalone SASL codec.
  Future discovery must validate the configured issuer before external requests,
  close/drain the discovery socket before interactive token acquisition, and pin
  reconnects to the same selected endpoint/security policy. Never send bearer
  credentials over plaintext or downgrade/fail over after authentication failure.
  Combined GSS/OAuth gates must query backend encryption separately from the
  PostgreSQL authentication method; the statistics principal is not evidence of
  a transport identity when OAuth authenticates. Once discovery used GSS, even
  an initial prefer policy must require it on reconnect. Declining encryption
  must expose no startup/token bytes and must not try another configured host.
  Keep third-party libpq controls in a separate process; never disable or suppress
  Weave's leak checking to hide a baseline leak. Backend cancellation controls
  observe the target query's actual active state instead of assuming a sleep
  makes it ready for cancellation.
  Use proven JSON/HTTP parsers, not ad hoc protocol parsing.
  OAuth custom-provider startup uses a protected empty-token discovery exchange,
  a separate bounded acquisition deadline, and an endpoint-pinned reconnect.
  Reject ambiguous decoded JSON keys and issuer retargeting before provider calls;
  discovery/rejected exchanges cannot become successful. Never confuse the test-only
  opaque-token validator or a libpq functional control with native HTTPS/device-flow
  qualification. Keep PostgreSQL server headers, validator libraries and libpq test
  controls strictly opt-in and private; library-only consumers need none of them.
  Native OAuth HTTPS uses Weave TLS and proven, privately embedded HTTP/URI parsers.
  Prefix external C parser linkage, suppress unintended DLL exports, install their
  licenses, and require C only for selected PostgreSQL source builds, not installed
  consumers or other module-only builds. Never export native parser headers/targets.
  Keep issuer identity literal, reject ambiguous numeric hosts and invalid framing,
  bound headers/body/total wire bytes, and never silently follow redirects or decode
  unsupported encodings. Fresh connections do not inspect every later unread record;
  do not describe a coalesced trailing-byte check as exhaustive stream validation.
  Transport gates alone do not qualify discovery, device grants or a real IdP.
  Native device providers retain owning prompts/client secrets and callable state
  through cancellation and unstarted Tasks. Client-secret copies share immutable
  credential allocations, cleansed on last-owner release; never turn Options copies
  into untracked plaintext clones. Parsed secrets reach owning provider requests;
  request credentials precede provider defaults, while explicit none rejects secrets.
  Validate confidential request credentials before metadata I/O. Keep keyword/URI
  decoding and service-file growth in cleansing buffers, including malformed input.
  Validate literal metadata issuer and
  advertised HTTPS endpoints before sending credentials; form-encode Basic inputs
  before base64 and never switch methods after failure. Charge device-request
  latency against code expiry, including prompt work, wait before every token poll,
  persist slow_down/timeout backoff, and never display private device codes.
  Keep HTTPS and PostgreSQL TLS/ALPN policies independent. Synthetic IdP and opaque
  validator controls are not real identity-provider deployment or JWT qualification.
  Real-IdP release gates use verified, pinned tool archives, fresh owned deployments,
  actual device approval and signed-token introspection. Keep PostgreSQL server,
  libcurl/json-c validator and JRE dependencies private and opt-in; never replace
  the real deployment with opaque tokens, a password grant or weaker TLS checks.
  Drain owned process groups and delete their credentials/datastores on every exit.
  Deployed Keycloak qualification does not make Weave a JWT validator or security audit.
  Configuration parsing stays synchronous and side-effect-free.
  LDAP service lookup is an explicit synchronous Options::load opt-in and an
  optional native build dependency, never hidden worker I/O. Preserve anonymous
  plaintext policy, disabled referrals/aliases, bounded values and native timeout
  limits; do not claim those limits preempt DNS or bound native PDU allocation.
  Only pre-search unavailability permits service fallback. Search/protocol/policy
  failures are terminal; loaded values still undergo normal security validation.
  Explicit OAuth well-known identifiers derive a literal issuer and pin the exact
  discovery URL; never canonicalize identifiers or accept middle-position forms.
  Cache lookup is synchronous, memory-only, owning and application-controlled.
  Invoke it only after a protected transport advertises allowed OAUTHBEARER, and
  recheck cancellation before sending. A miss preserves discovery/reconnect;
  lookup failure or token rejection is terminal, never implicit refresh or failover.
  Distinguish absent and explicit-empty configured scopes. Retain cache callable
  state through invocation; provider copies do not synchronize mutable caches.
  Keep environment/service/password-file loading explicit, own decoded fields,
  bound input and host counts, and reject unsupported settings instead of silently
  ignoring them. Do not map insecure libpq TLS modes to a different meaning.
  Configuration loading is a separate synchronous setup API: connection strings
  override services, which override environment defaults. Bound regular-file reads, check Linux
  password permissions on the opened descriptor, and own per-host secrets.
  Do not retain other destinations' passwords in the selected live session or
  make parse/connect silently consult ambient files. Windows paths are UTF8 and
  native wide paths; document reliance on deployment ACLs rather than claiming
  ACL validation. Keep shared option/secret helpers private to the module.
  System service discovery uses a private UTF8-byte-encoded compiled directory,
  configurable at build time and overridden only during explicit Options::load.
  Never infer it from the executable, invoke pg_config at runtime, or mutate real
  system files in tests. Keep automatic user/system discovery independently
  disableable; explicit paths and opted-in environment overrides remain usable.
- PostgreSQL local hosts name directories; append the bounded PostgreSQL socket
  filename and use Local transport, never a blocking-connect fallback. Require
  explicit plaintext rather than silently ignoring TLS policy. Reject ambiguous
  hostaddr and nonzero TCP-only tuning. Check required Linux kernel UIDs before
  startup; retain and recheck the actual peer UID and address for cancellation
  secrets. Resolve username requirepeer only in explicit Options::load, with bounded
  reentrant NSS forward/canonical reverse lookups. Freeze the authorized UID for
  connection/reset; reload explicitly after account-policy changes. Never hide NSS
  work in parse/connect or silently fall back after identity failure. Windows
  username/UID peer policy remains unsupported.
- PostgreSQL encodings follow reported ParameterStatus, never a hidden local override.
  Typed setters are real Tasks and retain a Connection borrow before initial suspension;
  confirm reported encoding under the exchange guard before success. Keep validation,
  quoting and conversion distinct: legacy validators follow PostgreSQL framing, not
  assigned-character tables. Copy complete multibyte characters, reject NUL and quote
  continuation bytes, and bound fully escaped output. Standalone helpers require the
  actual client encoding; preserve server conversion/SQL_ASCII errors. UTF8 column
  width uses installed ICU data, not a promise of libpq Unicode-table equivalence.
- PostgreSQL notice handlers are owning, move-only and noexcept. Deliver validated
  server notices inline on the executing connection graph, not on helper threads;
  diagnostics are borrowed only through invocation. Reject registration changes
  while deferred/active operations or session leases exist. Return the previous
  owning handler for explicit save/restore; empty registration restores the bounded
  queue. Retain the same receiver across startup, host attempts and reset, including
  failed resets. Do not retain it in query results or support reentrant session work.
  Reset Tasks acquire their Connection lease before initial suspension and release
  their own lease before replacing the old Impl; never leave a deferred reset
  borrowing an unprotected Connection or release through a destroyed implementation.
- PostgreSQL notification handlers own a move-only noexcept callable and consume
  newly decoded valid notifications instead of appending them to the bounded queue.
  Registration never replays or discards existing queued events. Explicit idle
  wait_notification still returns its owning event and invokes the handler once
  for newly decoded data; draining an older queued event does not invoke it again.
  Do not introduce hidden readers, subscription replay, polling or helper threads.
  Retain receivers across reset/failover and reject reentrant cancellation or
  registration while invoked/borrowed. Callbacks do not make Connection thread-safe.
- PostgreSQL tracing is opt-in and metadata-only by default. Startup, authentication
  and backend cancellation-key bodies never reach observers, even after ReadyForQuery
  or on malformed semantic messages. Application payload disclosure is explicit and
  bounded; it is not SQL/row/parameter sanitization. Trace complete logical frames,
  not encrypted records or acknowledged writes. Keep callbacks owning, noexcept,
  synchronous and serialized per receiver through duplex traffic and reconnects;
  never hold the receiver lock across an await. Retain observers before initial
  suspension and reject replacement with session leases/active callbacks. Disabled
  tracing must not add wrapper Tasks, transport buffers or receiver allocation.
- PostgreSQL lifecycle observers use owning noexcept callbacks and separate
  connection/result application-state slots. Retained results hold registrations,
  not a live Connection or its data registry. Moves transfer ownership without
  creation/copy notifications; copies let each accepted observer explicitly clone
  or share its state. Rejected registration/create/copy paths release owning data
  without later destroy hooks for that rejected instance. Reset emits only on
  success, and observer errors must not veto reset or interrupt cleanup. Keep
  per-registration callback serialization, immutable-source concurrent copies,
  synchronous busy-checked state access and no observer locks across awaits.
  Forbid nested lifecycle dispatch before locking; do not fabricate ResultSets
  for raw rows, COPY-format values or Task/Result errors merely to mimic libpq.
- PostgreSQL selective ResultSet copies are synchronous owning values. Rows imply
  columns; preserve kind, command, parameter types and suspended state regardless
  of payload selection. Populate selected fields before invoking copy observers.
  Disabling observers retains no registration/data owners and emits no copy/destroy
  callbacks for the new value. Never bypass the source's active lifecycle-dispatch
  contract, even with observers disabled. Hooks select their own state-copy policy;
  allocation failure follows the existing exception-disabled policy, not hook veto.
- PostgreSQL owning result containers are not exact standard-container types.
  Keep owning conversions explicit so lazy const-reference coroutines cannot
  silently borrow conversion temporaries. Prefer string_view/span for borrowed
  boundaries and explicit standard-container copies for owning ones. Text substring
  and concatenation return ordinary owning std::string values. memory_size is a
  synchronous diagnostic over distinct reachable retained pools, not RSS or an
  exclusive total; it may allocate temporary bookkeeping and requires coordinated
  graph mutation. Foreign pool counters are individually sampled. Keep physical
  footprint separate from logical delivery limits and opaque observer allocations.
- PostgreSQL attach_events is synchronous and uses the supplied idle Connection's
  current registry on an already populated ResultSet. Keep accepted registrations
  even with empty data, retry only absent/rejected IDs and preserve the first failed
  Result even when its error_code is zero. Continue other observers without rollback.
  Reject session borrows and nested lifecycle dispatch before locks/callbacks;
  retain accepted receiver/data owners, never Connection pointers or a pending registry.
  Do not infer result kinds, synthesize error results or alter automatic wire-result hooks.
- PostgreSQL ResultSet kinds come from protocol transitions, never row counts,
  column presence or command-tag guesses. Publish the kind before lifecycle
  callbacks and preserve it through copies/moves. Keep descriptions, empty queries,
  tuple completions, partial chunks and native acknowledgments distinguishable.
  Default query/execute Tasks still propagate SQL errors. Explicit outcome-returning
  methods own successful results and SQL diagnostics, drain ReadyForQuery before
  returning and retain deferred-task leases. Transport/protocol/cancellation/resource
  failures remain Task errors, never fabricated successful ResultSets. Bound retained
  diagnostic/outcome storage and reject duplicate errors or extra extended outcomes.
- Standalone PostgreSQL portal description owns its name and acquires its
  Connection borrow before initial suspension. Accept exactly one RowDescription
  or empty NoData response; never fabricate execution/command results. Preserve
  server-reported formats and ReadyForQuery/error recovery without fetching.
  Pipeline description remains a queueing operation with its existing lease.
- PostgreSQL pipelines explicitly lease one session. Queueing and Sync are
  synchronous; flush drives bounded concurrent sending/reading, appends backend
  Flush and never adds implicit Sync. Correlate every result and barrier, retain
  abort-until-Sync across flushes, and do not confuse protocol recovery with
  transaction rollback. Count unconsumed results against admission and retained
  data bounds. Pipeline Tasks retain leases before initial suspension; finish
  requires acknowledged synchronization, consumed results and no deferred/active
  Tasks. Never hold a pipeline mutex across an await or error propagation.
  Row chunks own their metadata and do not release command admission; only the
  complete event does. Bound queued deliveries and reader staging, reclaim charges
  on consumption, and never wait for the reader to reclaim its own oversized row.
  Charges travel with delivery ownership, including unstarted publication Tasks;
  drain queued deliveries before destroying their budget and wake channels.
  Streaming flush requires a progressing, joined consumer; consumer failure must
  cancel and drain a backpressured producer. Blocking start owns a lazy Context
  driver, next supplies progress and joins at EOF, and destruction cancels/drains.
  Keep buffered blocking flush semantics unchanged; never add a helper thread or
  imply that starting a driver is a transport-only flush acknowledgement.
  Split send acknowledges only transport writes; receive produces correlated
  results without writing. Keep one sender and one receiver, reject mixing with
  duplex flush, and preserve later submitted Sync debt when an earlier barrier
  completes. Receive-first may reserve a bounded batch for its partner sender;
  keep both joined and cancel/drain on consumer failure. Blocking split mode
  permits one submitted window and rejects unsent reads; use duplex start for
  bulk work rather than blocking a sender behind an unread result stream.
- PostgreSQL request_flush queues only a bounded backend Flush marker. It is
  synchronous, creates no correlation ID/result/admission slot and adds no Sync.
  Send byte-only snapshots, guard empty entry collections before back(), and
  require queued/reserved bytes to drain before finish. Keep automatic trailing
  Flush and existing SQL/barrier debt unchanged; Flush never clears abort state.
  On Runtime, keep split transport directions inside one when_all producer graph;
  separate scope-spawned roots are not serialized and must not share Connection
  protocol state. The independent result consumer only touches pipeline channels.
- PostgreSQL COPY BOTH has independent directions. Serialize complete sends
  without blocking its single reader; receive CopyDone must not silently close
  application sends. finish_copy_send closes only sends; end_copy drains terminal
  results before reuse. Preserve all replication completion results and bounds,
  retain deferred-task leases, and make active stream failures terminal rather
  than racing a sender with Sync recovery. Raw replication is not a WAL decoder
  or durability/acknowledgement policy; callers own those responsibilities.
- PostgreSQL mixed query/COPY exchanges lease one session until explicit synchronous
  finish after ReadyForQuery. Yield owning wire-order events without synthetic command
  tags or implicit WAL acknowledgements. Deferred COPY reads, writes and completion
  Tasks capture their phase generation before initial suspension; never retarget them
  to a later COPY. Publish send-half closure before awaiting its native completion,
  because the peer's terminal response can arrive first. Only tolerate structurally
  valid replication keepalives after receive CopyDone; arbitrary late data is invalid.
- PostgreSQL exchange RowOptions applies to the whole simple-query exchange.
  Zero keeps buffered results; positive chunk_rows is an upper bound, not an exact
  delivery size. Publish owning row_chunk values with schema before command completion,
  then a zero-row tuples result carrying the actual command tag. Reserve staging
  and delivery schema charges, emit early under retained-data pressure, and reject
  a single row/schema that cannot fit. Discard undelivered rows on SQL error and
  drain ReadyForQuery before recovery. Delivered chunks are not query success or
  rollback; preserve deferred-task leases, COPY phases and explicit finish.
  Cancellation before a reader starts must not consume a pending completion or
  retire a session that the reader has not advanced.
- Sync waiters live in coroutine frames. Register cancellation before publishing
  them, arbitrate grants/delivery/close/cancel under the primitive lock, and post
  through the captured executor. Disarm callbacks outside the lock before reclamation.
  Close wakes but does not join; primitives must outlive waiters and owned permits.
- TLS uses the proven OpenSSL engine, default peer/hostname verification and
  authenticated close_notify EOF. Serialize engine calls, retain write buffers
  through retries, and never hold the transport send gate while waiting for input.
  Reads with no outgoing TLS records must not join an unrelated pending send:
  duplex backpressure can require reads to make that send complete. Writes and
  shutdown still join the send gate even when another flush took their records;
  an empty output BIO is not proof that those records reached the transport.
  Cancellation after TLS state advances is terminal; drain before destruction.
  TLS shutdown sends network records and is asynchronous, unlike TCP half-close.
- TLS credential policies are immutable snapshots. Require explicit client trust
  for mTLS, enforce configured CRL/OCSP policy, and bound handshake/buffer resources.
  Sessions are opt-in, name/credential-bound, locally lifetime-capped and one-shot;
  resumed handshakes revalidate certificates. Never combine enforced OCSP with
  resumption without fresh-evidence semantics. Required per-handshake ALPN offers
  only the client's required label and checks actual selection on both roles before
  exposing application I/O. Never mutate credential snapshots; bind session offers
  to that label and the per-handshake SNI policy; reject mismatches before consuming
  one-shot sessions. Disabling client SNI suppresses routing disclosure only, never
  certificate or DNS/IP verification. Keep this policy on the SSL object, not shared
  credentials, and snapshot it for PostgreSQL cancellation before initial suspension.
  Cache clearing does not revoke
  stateless tickets; rotate credentials instead. No early data. Only explicit
  TlsVerification certificate/none and PostgreSQL weak TLS modes relax client
  verification; never infer them from failure or implicit ambient reads. Unverified
  peers are not exported as authenticated identities, and metadata distinguishes
  encryption, certificate verification and hostname verification. Verification-none
  rejects enforced revocation/OCSP and resumption rather than pretending to enforce them.
  PostgreSQL libpq_compatibility is an explicit loader-only migration profile;
  ordinary parse/connect retain secure defaults and no hidden source discovery.
  TLS key logging is explicit, sensitive, synchronous append-only NSS output to
  an owner-private regular file. No ambient SSLKEYLOGFILE opt-in, automatic logger,
  secret-path configuration export or silent setup/write failure. Preserve file
  and credential ownership through concurrent handshakes and stream destruction.
  Require supported security-patched OpenSSL and rerun docs/tls-release.md gates
  for security-sensitive changes; never describe automated tests as a security audit.
  Cleanse owned private-key passphrases on factory validation/provider failures as
  well as identity loading. Best-effort owned-buffer cleansing does not erase
  caller copies, all allocator history or compiler/third-party temporaries.
  TLS key-passphrase providers own const noexcept callables and run synchronously
  only when the native encrypted-key decoder requests a secret. Copies share the
  callable, not a callback lock; applications synchronize captured mutable state.
  Never retain provider/loading-frame pointers or returned passphrases in completed
  credentials, truncate secrets or fall back to stdin after failure. Preserve failed
  Result state even for error-code value zero. PostgreSQL owns provider copies before
  initial suspension and distinguishes credential setup failure from endpoint
  availability: callback timeout, disconnect and target-session errors must not
  cause host failover, including ping and reset.
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
