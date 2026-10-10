# Result Storage

The owning storage model is integrated into ResultSet. `memory_size()` has a
selected ownership/query contract and permanent nine-profile regression, native,
real-server and packaging qualification. [Matched native Windows measurements](postgres-benchmarks.md)
now include nine retained-memory shapes and serial/concurrent timings;
this is not complete PostgreSQL parity or whole-module production certification.
The internal allocator/provider types are not supported
public APIs. [Current result API](postgres-results.md).

The development records below preserve the sequence of decisions and failures.
Their outstanding-promotion notes are superseded by the
[permanent qualification](#permanent-regression-qualification-2026-10-10).

## Measurement Boundary

The candidate counts actual backing-heap requests for owning result storage:
arena objects, allocation headers, alignment padding, container allocations and
debug iterator/proxy allocations. It does not infer requests from capacity or
call platform heap-size APIs. It does not measure RSS or allocator usable size.

The backing store is an owning retained pool. Individual container deallocations
end object lifetimes but retain the actual backing blocks until the last pool
owner dies. Clearing/shrinking a container therefore need not reduce the pool
footprint. [libpq's implementation](https://github.com/postgres/postgres/blob/REL_18_STABLE/src/interfaces/libpq/fe-exec.c)
similarly reports retained malloc requests for PGresult storage rather than live
field lengths. Its event userdata is outside that accounting.

Pools are counted once per result graph, including foreign pools imported through
public mutable fields. Shared pools can appear in several results, so adding
per-result totals is not an exclusive-ownership or process-memory total. A pool
may retain blocks for data moved out of a result while that result still owns the
pool. Those are actual retained blocks, not guessed live payload sizes.

Shared receiver objects and opaque application event data remain
outside the per-result storage boundary. Result-local registration arrays are
included, along with the separately allocated result-local event node. The
containing caller's stack/heap allocation and temporary inspection
scratch are not result-storage requests. Those boundaries must remain explicit;
the metric does not count every library/application allocation.

## Lifetime Model

Allocator handles own reference-counted pools, rather than borrowing a resource
embedded in ResultSet. Each active allocation also retains its original pool.
Its header identifies the owner for deallocation, including allocator rebinding
and standard-library debug-proxy transfers. Ending an allocation drops that lease;
the last handle/allocation frees every retained block and the pool object.

This keeps independently moved strings, rows and vectors valid after the result
owner dies. Alignment and multiplication/addition bounds are checked before a
backing request. Allocation failure follows the existing exception-disabled fatal
allocation policy, not an invented recoverable Task error. The custom backing-heap
interface is currently a borrowed test fixture; it is not a public provider API.

Pool bookkeeping is synchronized for shared allocation/destruction. Mutable
containers still require external coordination. Read-only copies create independent
result pools; they do not share mutable fields. A result-shaped clone preserves
metadata, NULL/empty/binary values, command tags, parameter types and suspension.
Observer entries retain external owners without pretending to measure their
opaque allocation history.

## Compatibility Prototype

The prototype includes standard-container-backed text/list adapters. Controls
cover aggregate Column/Value initialization from std::string, optional text,
literal/string/view comparisons, ordinary mutation, std::span construction and
standard-vector assignment/conversion. Converting between differently allocated
owning standard containers can copy data; this is not a zero-cost ABI bridge.

The actual public result fields now use these adapters. Column/Value remain
aggregates, Row remains the named row type, and streaming column callbacks keep
their std::vector<Column> signature. Parser/delivery paths, selective copies and
event bookkeeping have been migrated. The concrete string/vector types and ABI
have changed: this is not full source compatibility, especially for references
to exact standard-container types or templates constrained to those exact types.
Result text now supports standard string-style formatting and hashing; that
does not turn it back into std::string or establish ABI compatibility.
The original model controls remain separate from the actual-library probes below.

The selected diagnostic query walks result fields and uses temporary bookkeeping
proportional to distinct pool identities. That scratch is not result storage;
the query may allocate and follows the exception-disabled fatal allocation policy.
It does not perform I/O or create a Task. Mutable graph inspection requires external
coordination. Shared pool counters are individually sampled, not one atomic snapshot
of concurrent allocations across all foreign pools. The owning backend is not a
credential allocator or a replacement for existing cleansing policy.

## Confirmed Controls

Three successive prototypes pass Windows/Linux Debug, Release and ASan profiles.
The final candidate covers backing-request totals against a separate injected
allocation ledger, growth/clear/shrink, escaped storage, copies/moves/swaps,
256-byte alignment, four-thread shared allocation/destruction, foreign pools,
immutable concurrent copies, observer boundaries and standard-container idioms.
All observed backing requests are released when the last owner/allocation dies.

Separate libpq-only controls use native 18.4 on Windows and 18.6 on Linux. They
cover empty-result footprint, zero-size allocation, schema/tuple construction,
NULL replacement retaining storage, larger subsidiary allocation, independent
copies and copied-result lifetime after the original is cleared. These are
semantic controls, not equal-byte comparisons or performance benchmarks.

Across the three runs, exact nonempty inventories/JUnit verify **36 CTest cases**,
**18 storage-control processes** and **18 native-control processes**, without
failures, skips or errors. Native project references/Ninja commands confirm
libpq-only linkage with exceptions disabled and no sanitizer instrumentation.
The storage controls retain ASan instrumentation; Linux includes leak detection.
First-party production sources, HEAD, the staged patch and protected ASan runtime
are unchanged during these runs.

| Ignored Archive | SHA-256 |
| --- | --- |
| postgres-result-memory-prototype-v1-20261009.zip | b2592c147222f17a3788b9ec05f0c6e493ab68a4a55ecddd68461482339c27af |
| postgres-result-memory-prototype-v2-20261009.zip | db16dbb8b54bd7c9435c64cfe8621698e71e03b38e3ea14e39918625da978e13 |
| postgres-result-memory-prototype-v3-20261009.zip | f1c08183d2d2ee25e8172292316cb87a53c7770de0d06f0871c8c1970aa7457f |

Independent verification checks archive CRC/member hashes, exact inventories,
JUnit, positive scope/version markers, frozen sources and native dependency
boundaries. The numeric totals come from allocator requests, not container
capacity; the evidence does not qualify the actual PostgreSQL delivery engine.

## Integration Bar

The model is wired through parsed/buffered/chunked/portal results, retained SQL
outcomes, selective copies and result-local event state. Before promoting it,
qualify every producer and compatibility boundary. Preserve Task/Result behavior,
leases, cancellation, limits, wire semantics and externally coordinated mutation.
Confirm allocation failure/overflow guards and every storage lifetime boundary,
including callback observations and foreign/moved-out storage.

Then lock the API, add permanent regressions, run the full affected-source,
native, real-server, reduced-module and relocated-package matrix, and include
allocation/memory behavior in the final matched libpq measurements. No shipping
memory contract, complete libpq parity, performance improvement or production/security
qualification is claimed by the development implementation. PQresultMemorySize
is now Partial, not Mapped.

## Actual-Library Integration Controls

The latest candidate passes six storage profiles: Windows Debug/Release/ASan
and Linux Debug/Release/ASan. These probes link the actual PostgreSQL implementation,
not the earlier result-shaped model. Independent heap request/free records match
ResultSet's reported bytes for explicit fixture-owned results, full/selective
copies, copy assignment, foreign pools and escaped rows/text. Four threads each
make 64 immutable copies; all fixture-owned requests are freed after final cleanup.

Separate synthetic-peer probes stream 1,024 rows per session under a 2,048-byte
logical retained-data limit. They check owning values, copies, lifecycle callbacks,
distinct successive row pools and bounded per-chunk footprints. Context and blocking
drivers use one session; each Runtime configuration uses four workers and 16 roots.
Both schedulers run on each platform, with both IOCP layouts on Windows.
The graph is synchronous/read-only; these are correctness checks, not timings.

Independent verification confirms **22 CTest cases**, **six ledger processes**,
**30 streaming processes**, **300 sessions** and **307,200 streamed rows**.
Six CTest entries cover the new storage probes; 16 cover eight affected regressions
on each platform's full-feature Debug build. The scratch probe builds disable
optional GSSAPI/LDAP; they do not establish runtime-disabled or relocated-package
qualification. Windows ASan uses Release; Linux ASan uses Debug with leak detection.
Loaded/header OpenSSL identity is checked: Windows 3.6.5 and Linux 3.5.5.

A post-sweep default-container check found a real MSVC Debug accounting defect:
empty containers allocated iterator proxies through a rebound temporary whose
pool was not reachable through the source allocator. Proxy-enabled allocators now
establish shared storage before that copy. Release containers retain lazy storage.
The failed control is archived, and the complete six-profile sweep is rerun after
the fix. Allocator moves also preserve the source handle until source reset, because
standard-library proxies can remain in moved-from containers.

Schema and row storage are separate. Exchange publication resets its row allocator
instead of merely clearing rows. Pipeline reservation can publish a batch while
the next row is being prepared; that row is reparsed into the replacement batch's
pool rather than retaining the published batch. Existing protocol/limit/cancellation
regressions pass, but dedicated accounting controls for every producer still remain.

Evidence is retained in ignored
`postgres-result-memory-integration-v1-20261009.zip` and
`postgres-result-memory-integration-verification-v1-20261009.json` under
benchmarks/results. Frozen source/member hashes, executable hashes, exact nonempty
inventories/JUnit, HEAD, the staged patch and the protected ASan DLL are checked.
Earlier fixture failures and the proxy regression failure remain diagnostic evidence,
not passing cases. No new native-libpq qualification or timing run is claimed here.

## Compatibility And Producer Controls (2026-10-10)

A real compatibility probe failed before this correction: C++23 formatted
ResultText as a character range, so string width/precision specifications failed
to compile; std::hash<ResultText> was unavailable. The installed implementation
header now supplies a string-view formatter and hash specialization. Formatting
forwards the standard string-view parser/state, and hashing includes the complete
view, including embedded NUL bytes. No new throwing Weave API or formatting engine
is introduced. Conversion to std::string still produces a separate owning copy.

Six actual-library profiles pass 17 interoperability checks each, covering
string formatting, width/precision, hashing/unordered maps, binary text, ostream,
optional/string conversions and ordinary views/mutation. Private backing-provider
creation also rejects missing allocate/deallocate callbacks before calling them.
Each profile confirms ten expected fatal contracts and a valid allocation/free
control: pool/block/copy allocation failure, absent callbacks, count/size overflow,
invalid/zero alignment and double deallocation. These are checked abort contracts,
not recoverable allocation errors; inaccessible counter-overflow branches are not
claimed covered. The provider remains a borrowed private fixture.

The post-correction sweep independently verifies **18 CTest cases**, six backing
request/free ledgers, six interoperability processes, six guard drivers and
**60 expected contract failures**, plus six valid controls. The owning synthetic
streaming sweep also passes again: **30 processes, 300 sessions and 307,200 rows**,
with the same four-worker schedulers, Windows IOCP layouts and logical limits.

Separate owned PostgreSQL 18 server runs pass on all six profiles. Each observes
**2,050 result inspections and 33,630 checks** across ordinary/extended/prepared
queries, descriptions, portals, batches, pipelines, COPY OUT completion, mixed
exchanges, reset and retained SQL failures. Context and blocking paths exercise
explicit plaintext and verified mTLS/SCRAM-PLUS; each scheduler runs 32 independent
roots on four workers, alternating those security profiles. Reset can open further
connections, so root counts are not presented as connection counts.

That producer oracle independently walks distinct pools on deep copies of actual
delivered results, compares their compositional footprint and checks original
footprints/metadata remain unchanged. It is not backing-heap injection into every
wire producer, and not an all-producer accounting qualification. That checkpoint
did not cover dedicated replication completion, standalone raw-row streams or
pressure-driven pipeline accounting; the next checkpoint adds those controls.
Windows Release and Linux Debug relocated component
packaging each pass one CTest case, bringing this checkpoint to **20 CTest cases**;
this does not replace the runtime-disabled or full optional-provider matrix.

Frozen input archives, executable/source hashes, exact inventories/JUnit and
protected HEAD/index/ASan state are checked independently. The original formatting/
hash compile failure is retained as diagnostic evidence. The ignored checkpoint is
`postgres-result-memory-compat-v1-20261010.zip`, with
`postgres-result-memory-compat-verification-v1-20261010.json` under benchmarks/results.
Temporary probe sources are archived and removed, not promoted to permanent tests.

The concrete-type/ABI and temporary-query-allocation contract still need a final
decision before API lock. Permanent regressions, the full native/release matrix
and final matched libpq allocation/memory/performance measurements remain required.
No new native-libpq run or timing measurement is claimed; PQresultMemorySize stays
Partial, and this development checkpoint is not a production/security audit.

## Producer Ownership Controls (2026-10-10)

Dedicated pressure-pipeline and raw-row/replication probes now pass Windows/Linux
Debug, Release and ASan without changing production code. An independent verifier
checks six nonempty CTest cases and six owned PostgreSQL 18 fixture processes,
archived inputs, matching executable hashes and unchanged first-party/protected
state. No timing or new native-libpq qualification is included.

Pipeline controls exercise **384 streams and six abandoned-delivery controls**,
including **84 pressure-driven streams with 1,572 early chunks**, **132 cancellation
cases** and **66 consumer failures**. The pressure case requests up to 37 rows per
chunk under a 1,024-byte logical bound; actual chunks arrive earlier. Every chunk's
rows and values share its distinct owning row pool. All earlier pool handles are
retained deliberately: their backing footprints stop growing as subsequent chunks
arrive. This catches a prepared row retaining an already published batch's pool.
It is a per-pool ownership/retention control, not a total physical-memory quota.
SQL failures, oversized rows, queued/byte pressure, split/duplex drivers and terminal
cleanup are also covered. Four-worker runtimes use both schedulers and both Windows
IOCP layouts, with 16 independent roots per configuration.

Real-server controls exercise **168 raw-row streams containing 21,504 retained rows**
and **168 physical-replication streams**. Raw rows have independent pools and preserve
ordinal, long text, NULL and empty values after later rows, query reuse, reset,
finish and connection destruction. Moving a value's text out remains valid after
all retained rows/pool handles are cleared. Replication preserves both actual
START_STREAMING/START_REPLICATION completion results, checks result-copy accounting,
and retains completion tags after reset/destruction and final owning-result cleanup.
Both async and blocking paths run over plaintext and verified mTLS/SCRAM-PLUS.
Runtime configurations use four workers/eight roots; Windows exercises both IOCP
layouts, Linux only sharded. Counts describe streams, not all connections created
by reset. Raw-row column metadata is an owning copy, not a borrowed view whose
lifetime has magically been extended.

The real-server oracle still composes independently walked pool totals on deep
copies; the backing-request injection ledger remains a separate control. It does
not globally intercept allocations from every wire producer. Across these probes,
there are **3,702 pipeline and 23,016 real-server result inspections**, with
**2,285,124 checks**. Optional GSSAPI/LDAP are disabled in the scratch profiles;
Windows ASan is Release, Linux ASan is Debug with leak detection. The provider
dependencies are not all sanitizer-instrumented.

Two initial fixture builds failed because of a wrong certificate-header name and
a missing port-helper include. They are retained as attributed diagnostic failures.
An earlier passing exploratory run is explicitly not qualification: references to
arena members outlived temporary get_allocator() objects. The final probe copies
owning arena handles by value and the complete six-profile sweep is rerun. This
was a probe defect, not a demonstrated production lifetime defect; the earlier
integration/compatibility controls use owning values or full-expression calls.

Evidence is retained in ignored `postgres-result-memory-paths-v1-20261010.zip` and
`postgres-result-memory-paths-verification-v1-20261010.json`; primary probe sources
are archived and removed. PQresultMemorySize remains Partial pending the public
container/ABI and query-allocation contract, permanent promotion, reduced/provider
and full release/native qualification, and final matched libpq measurements.

## Selected Container Contract (2026-10-10)

The concrete container/ABI change is intentional and is now selected for permanent
regression promotion. These fields are not exact std::string/std::vector references
or a drop-in ABI bridge. Normal read/mutation/view operations remain available,
but callers should use std::string_view/span at borrowed boundaries and explicitly
materialize standard containers at owning boundaries. Views still require their
source data to remain valid; neither an arena nor a Task extends an ordinary view's
lifetime automatically.

Result text supports standard formatting/hash and string/view/literal/character
concatenation. `substr()` and concatenation produce **ordinary owning std::string**
values, preserving lengths and embedded NUL bytes. Their allocations belong to
those independent outputs, not the original result's retained pools. Standard
container methods can still expose allocator-specific base types; this is not an
assertion that every standard-string/vector template accepts the adapters.

Owning string/vector conversions are **explicit**. An implicit copy could otherwise
silently create a temporary when binding an existing `const std::string &` or
`const std::vector<T> &` API; a lazy coroutine could borrow that temporary after
the construction statement ends. Static compiler guards reject those conversions.
An explicitly constructed temporary can still be borrowed incorrectly: keep an
owning local through completion, or use a coroutine's by-value parameter. The
pipeline's schema cache now explicitly makes the same owning vector copy it made
before; protocol, quota, allocation-provider and scheduling behavior are unchanged.

```cpp
std::string_view borrowed = result.command; // Only while its source remains valid.
std::string owned{result.command};
auto prefix = result.command.substr(0, 6);   // std::string, independently owned.
auto label = result.command + std::string{" completed"};
auto columns = static_cast<std::vector<weave::pg::Column>>(result.columns);
```

The finalized candidate passes six **Runtime-omitted** Debug/Release/ASan profiles,
with **30 interoperability checks per profile** and both Context/blocking drivers
over plaintext and verified mTLS/SCRAM-PLUS. There are **24 raw-row streams**,
**3,072 retained rows**, **24 physical-replication streams**, **3,288 result
inspections** and **289,440 producer checks**. Project references/native link commands
confirm Runtime is neither built nor linked, and exceptions are disabled.
Windows ASan is Release; Linux ASan is Debug with leak detection. Linux GSSAPI/LDAP
and Windows LDAP are disabled in these scratch profiles; Windows SSPI remains its
platform implementation. Provider dependencies are not all sanitizer-instrumented.

The final headers also pass **16 affected full-feature Debug regressions** across
Windows/Linux and **two relocated packaging gates** (Windows Release/Linux Debug),
bringing this checkpoint to **24 CTest cases**. The older baseline's two negative
string capabilities and intermediate implicit-copy design are retained separately,
not counted as final qualification. The first explicit-conversion build caught the
pipeline cache's implicit vector assignment; the attributed compile failure is
retained and the complete reduced sweep/regression/package gates rerun after its
migration. No undefined-behavior coroutine probe was executed or claimed passed.

Evidence is in ignored `postgres-result-memory-contract-v1-20261010.zip` and
`postgres-result-memory-contract-verification-v1-20261010.json`. Frozen source/input/
executable hashes, exact nonempty inventory/JUnit and protected state are checked
independently. Primary ad hoc sources are archived and removed. The memory boundary
and query/container contract are now selected; permanent regressions and the full
native/release matrix remain before promotion. PQresultMemorySize stays Partial,
and final matched libpq memory/performance measurements remain in scope.

## Permanent Regression Qualification (2026-10-10)

The confirmed probes are now module-owned permanent tests, not scratch sources.
Five default CTest cases cover an independent backing-request/free ledger, explicit
container conversions, expected fatal contracts, ordinary exchange chunks and
pressure pipelines. Optional libpq controls and disposable-server cases add native
growth/copy/lifetime checks and actual buffered/prepared/portal/batch/pipeline/COPY,
raw-row and physical-replication result producers. These are correctness tests;
they do not enable benchmarking or add a libpq dependency to the library.

Windows Debug/Release/ASan and Linux Debug/Release/ASan pass, as do Windows/Linux
Debug without Runtime and Linux Debug with Runtime/GSSAPI/LDAP disabled. Four-worker
affine/stealing schedules and both Windows I/O layouts are exercised where supported.
Eight native controls use loaded/header-matched libpq 18.4 on Windows and 18.6 on
Linux. Those controls are functional comparisons, not sanitizer instrumentation of
the external libpq/OpenSSL libraries or equal-byte assertions between allocators.

The complete verified gate contains **112 CTest cases**, including 48 existing
affected regressions and three isolated/relocated packaging cases. It also includes
18 owned disposable PostgreSQL fixture processes, 90 expected contract aborts,
306 exchange streams (313,344 rows), 432 pipeline streams, 90 pressure streams
(1,690 early chunks), 144 cancellations, 72 failing consumers, and 180 raw-row and
180 physical-replication streams. The compositional producer controls perform
41,674 inspections and 2,698,699 checks. Wire producers are not globally heap-injected;
the independent ledger and copied-graph walk remain distinct oracles.

An early runner pass incorrectly enabled LeakSanitizer on Windows. The instrumented
executables rejected that unsupported option before their test bodies ran; its
failed inventory/JUnit is retained. The corrected complete sweep uses Windows
AddressSanitizer without LeakSanitizer and Linux AddressSanitizer with leak detection.
No production-code change or skipped test was used to make that pass.

Frozen source/input archives, exact nonempty inventories/JUnit, executable hashes,
and unchanged HEAD/index/protected sanitizer DLL are independently verified.
Evidence is retained under ignored `benchmarks/results/` as
`postgres-result-memory-permanent-v1-20261010.zip` and
`postgres-result-memory-permanent-verification-v1-20261010.json`.
Only permanent tests and their CMake registration change from the selected candidate;
no runtime/protocol/storage implementation is redesigned here. Primary ad hoc
qualification scripts are archived and removed. PQresultMemorySize is now Mapped
with the nonexclusive retained-pool boundary explicit. Final matched allocation,
memory and performance measurements remain required for the wider objective.
