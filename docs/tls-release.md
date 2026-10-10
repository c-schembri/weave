# TLS Release Gates

The [2026-10-10 fixed-scope checkpoint](postgres-closure.md) supersedes older
pending-promotion notes below for Windows/Linux certificate/SNI, CRL directories,
PEM/DER/configured STORE keys and final matched measurements. Read its exact
platform results and retained failures before extending a readiness claim.
The dated sections below preserve the investigation history; their unfinished
notes do not create an expanding implementation backlog.

[Migration follow-on qualification](postgres-migration-qualification.md) covers
the later explicit verification modes and private key logging.
[The closed batch's scope](postgres-release.md) keeps deployment qualification
separate from further feature development.

The supported profile is TLS 1.2/1.3 over Weave's cancellable stream interface,
using a maintained, security-patched OpenSSL 3.5+ provider on Windows IOCP or Linux
io_uring. These release gates cover that profile, not every feature OpenSSL can
expose or an independent security audit. The adapter does not implement cryptography.

## Required ALPN Development (2026-10-10)

Per-handshake required ALPN is implemented without mutating credential snapshots.
Six Windows/Linux Debug/Release/ASan development profiles cover exact/binary
labels, both roles, TLS 1.2/1.3, stateful/ticket resumption, non-consuming policy
rejection and unchanged ordinary ALPN behavior. PostgreSQL transport controls
exercise the policy across startup/reset/cancellation, OAuth and four-worker
execution; [complete development scope](postgres-qualification.md#direct-tls-development-2026-10-10).
Permanent required-ALPN engine regressions and existing TLS suites now pass in
nine full/reduced profiles, alongside relocated TLS/PostgreSQL package gates.
PostgreSQL's endpoint-pinned reconnect refusal also has permanent coverage;
[regression scope and remaining integration work](postgres-qualification.md#direct-tls-regression-checkpoint-2026-10-10).
Synthetic direct-TLS startup/reset/cancellation/pinned-OAuth regression promotion
now passes nine profiles with packaging; [latest peer qualification](postgres-qualification.md#direct-tls-peer-regression-qualification-2026-10-10).
Permanent real-server mTLS/SCRAM-PLUS/reset/query-cancellation and independent
libpq controls now pass nine profiles; protected-GSS priority passes four Linux
profiles. [Server qualification](postgres-qualification.md#direct-tls-server-regression-qualification-2026-10-10).
Broader release/deployment qualification remains. Earlier snapshots do not silently
qualify untested additions or expand the supported security profile.

## Key Passphrase Provider Checkpoint (2026-10-10)

Owned, demand-driven private-key password callbacks are implemented for both
credential roles. Loading uses a temporary native callback frame; completed
credentials retain no password, provider or loading-frame pointer. Provider
errors remain failed Results even with code value zero. Overlong secrets fail
without truncation and callback failure cannot trigger PostgreSQL host failover.
The named `detail::TlsPasswordAccess` is a header-declared friend access hook;
its implementation is confined to engine.cpp, not shared runtime machinery.

Six frozen development profiles pass: Windows Debug/Release/ASan and Linux
Debug/Release/ASan. Probes cover owning move-only handlers, copied concurrent
providers, unused/conflicting/moved-from options, correct/wrong/overlong/empty/
embedded-NUL passwords, allocation cleanup and provider release before later
handshakes. TLS 1.2/1.3 mTLS runs on Context and four-worker runtimes with both
schedulers and every supported I/O layout. Six disposable PostgreSQL primary/
standby fixtures verify SCRAM-PLUS, blocking/Context startup, reset and four-worker
connections. Independent synthetic peers observe 240 credential-failure
connections, zero second-host connections and zero sent protocol bytes.

Existing required TLS suites and affected PostgreSQL startup/configuration/
authentication/cancellation/failure regressions also pass on all six profiles:
**73 nonempty CTest cases**, including four TLS/PostgreSQL component-package
cases on Windows/Linux Release. These tests retain the documented revocation,
sessions, record/duplex/lifetime and Python/OpenSSL interoperability coverage.
Windows ASan uses detect_leaks=0; Linux uses detect_leaks=1. OpenSSL itself is
not instrumented by these packages. Dependencies remain the tested Windows
OpenSSL 3.6.5 and Linux OpenSSL 3.5.5 installations, not every provider/deployment.

Earlier investigation failures are retained and attributed: scratch command
quoting/import visibility/missing includes/renamed-main compilation; an
address-only witness unable to observe MSVC's aligned large-string allocation;
an unpinned localhost report counting separate DNS addresses; a nonexistent
fixture database; peer shutdown before draining the listen backlog; and a gate
attempting a disabled Kerberos target. They are not passing evidence or unexplained
production failures. Complete corrected sweeps pass without production changes
to accommodate those harness failures.

Ignored evidence: `tls-password-provider-development-v1-20261010.zip` and its
verification JSON. HEAD, staged bytes and the existing ASan DLL stay unchanged.
Temporary investigation sources/fixtures are removed after evidence sealing.
At this development checkpoint, permanent provider-specific regression promotion, reduced-module controls,
independent libpq password-hook qualification and final matched measurements
remain pending. This checkpoint does not certify all PostgreSQL deployments,
opaque callback internals or complete secret erasure; no benchmarks are rerun.

## Permanent Provider Regressions (2026-10-10)

The TLS portion of the pending promotion now passes. `weave_tls_credentials`
includes owning/copyable/move-only callback, native decoder, failed-Result-zero,
validation, concurrency, lifetime and returned-secret cleanup regressions.
`weave_tls_password_streams` exercises provider-loaded TLS 1.2/1.3 mTLS credentials
after their callback owners expire, on Context and every supported four-worker
scheduler/I/O layout. Both tests assert that their owned encrypted-key fixture
directories are removed. Numeric TCP setup preserves separate TLS hostname
verification without introducing DNS fallback into this local correctness test.

All **18 nonempty CTest cases** pass on nine frozen profiles: Windows/Linux
Debug, Release and ASan, plus Windows without Runtime/LDAP, Linux without
Runtime/LDAP, and Linux without Runtime/GSSAPI/LDAP. Runtime remains optional;
reduced profiles retain Context mTLS and the same factory controls. These are
functional assertions, not performance measurements or instrumentation of OpenSSL.
The existing 73 development release/package cases remain separate evidence.

Ignored evidence is `tls-password-provider-permanent-v1-20261010.zip` and its
verification JSON. Only permanent test/helper sources and TLS test registration
change from the development implementation; production remains identical.
PostgreSQL-specific permanent promotion, independent libpq hook controls and
whole-module/final measurement work remain required. No staging, commit or push.

## PostgreSQL Provider Qualification (2026-10-10)

The PostgreSQL-specific promotion and native-hook controls above are now complete
for encrypted PEM keys. Nine full/reduced profiles pass **104 exact-inventory
CTest cases**, including existing affected TLS/PostgreSQL regressions and four
relocated component-package cases. Eight additional Windows real-server runs
bring the owned primary/standby fixture count to 18. Independent libpq 18.4/18.6
controls run in nine separate processes, without linking Weave. They cover
demand-driven key lookup, correct/wrong/overlong/denied passwords, unencrypted
keys, reset, hook restoration and concurrent connections.

Permanent Weave tests cover blocking/Context/four-worker startup, reset and
provider release, plus dropped/rejected lazy Tasks. Synthetic peers independently
observe 288 terminal credential-failure connections, zero second-host connections
and zero protocol bytes, including failed Results carrying error-code value zero.
Runtime, LDAP and GSSAPI remain optional. Neither production TLS nor PostgreSQL
source changes from the preceding provider implementation.

The final evidence combines passing commands on identical first-party bytes;
it is not a claim that the earlier orchestration rounds passed. Those failures
are retained. An existing one-second session-expiry test raced whole-second
capture boundaries; its failed REQUIRE continued under exception-free doctest,
then dereferenced the failed Result. Controlled probes confirm both conditions.
The test now guards failed setup and allows three seconds for capture before
testing actual expiry. A reduced-build test scope error and a missing minimal
profile's opt-in registration were also corrected without production changes.

Ignored evidence is `postgres-tls-password-permanent-v1-20261010.zip` and its
verification JSON. [Exact scope and accounting](postgres-qualification.md#postgresql-key-password-provider-regressions-2026-10-10).
Windows ASan has leak checking disabled; Linux Weave ASan enables it. Native
libpq/OpenSSL controls are functional baselines, not sanitizer-instrumented
dependencies. Broader deployments, independent security review and final matched
measurements remain required. No benchmarks, staging, commit or push occur.

## Required Evidence

| Gate | Validation |
| --- | --- |
| Peer authentication | SAN hostname/IP checks, chain purpose/time, trust rejection, mTLS absent/invalid/valid identities |
| Revocation | Clean/revoked CRLs, missing required OCSP, verified GOOD staple, malformed/revoked/expired staple rejection |
| Session policy | Disabled default, TLS 1.2/1.3 stateful and ticket resumption, name/credential binding, one-shot captures, mTLS resumption, cache invalidation |
| Record/lifetime safety | Fragmented records, authenticated EOF vs truncation, duplex I/O, same-direction rejection, pending cancellation and Context stop |
| Resource safety | Bounded input/output/certificate size, verification depth, malformed peer input, deadline cancellation/drain |
| Scheduling | Four workers with affine/stealing scheduling and every supported native I/O layout |
| Interoperability | Both roles and both TLS versions against Python/OpenSSL, not just Weave talking to itself |
| Toolchains | Windows and Linux Debug, Release and ASan correctness suites |
| Distribution | Isolated TLS-only build, relocated component consumer and self-contained public headers |
| Dependencies | Security-patched supported OpenSSL; exact tested version and package provenance recorded |

Tests run in correctness CI. Long repetitions and adversarial soak runs supplement
the bounded suite; benchmarks never become a release-blocking correctness job.
Archive diagnostic evidence in ignored build output, not tracked logs.

For a local sanitizer soak, repeat the TLS suite without retrying failed runs:

```sh
ctest --preset asan-tls -R weave_tls --repeat until-fail:10 --output-on-failure
ctest --preset linux-asan -R weave_tls --repeat until-fail:20 --output-on-failure
```

The second command runs inside Linux/WSL using its configured build preset.
The OpenSSL dependency itself is not ASan-instrumented by the default packages;
these gates instrument Weave and its fixtures, not all cryptographic internals.

## Deployment Responsibilities

- Configure authorization separately from certificate authentication.
- Select explicit trust/revocation policy where root-store defaults are insufficient.
- Refresh CRLs/staples and rotate credentials before certificate/evidence expiry.
- Bound accepted connections, handshake concurrency, application buffers and I/O time.
- Drain tasks before destroying streams, credentials' borrowers or their Context.
- Deploy patched OpenSSL runtime libraries and monitor upstream security advisories.
- Test required provider/FIPS, proxy, trust-store and certificate profiles in the
  actual deployment. This repository does not claim FIPS certification.

## Session Clock Correction

Supplemental qualification exposed intermittent Linux session-capture rejection.
An instrumented reproduction recorded coarse `time()` as `1791336734`, while
both high-resolution realtime and OpenSSL's new session reported `1791336735`.
The session was resumable, had its verified chain, and had a 600-second lifetime;
Weave incorrectly classified it as future-dated.

[OpenSSL's clock implementation](https://github.com/openssl/openssl/blob/openssl-3.5.5/crypto/time.c)
uses `gettimeofday` on Linux. The
[Linux vDSO implementation](https://github.com/torvalds/linux/blob/v6.18/lib/vdso/gettimeofday.c)
interpolates high-resolution realtime for that API, but `time()` reads the base
whole-second value. Mixing those samples can disagree at a second boundary.
Session capture, availability and remaining lifetime now use coherent
`system_clock` realtime. Future-date, expiry and certificate checks remain strict;
no extra lifetime or grace interval was added.

The corrected resumption matrix passed 1,000 Linux ASan process repetitions.
A permanent Linux-only linker-wrapped clock fixture deterministically failed
with the old implementation and passed 20 repetitions with the correction.
It isolates `time()` inside its own test executable/thread without changing the
OS clock, OpenSSL's shared-library clock, or production callbacks. Temporary
instrumentation is removed; failures are retained as diagnostic evidence.

After that correction, frozen source passed fresh Windows and Linux Debug,
Release and ASan builds and correctness selections, plus native Windows
PostgreSQL interoperability in all three configurations. Linux ASan passed
20 repetitions of three TLS entries (60 passes, 72.45 seconds); Windows ASan
passed 10 repetitions of two TLS entries (20 passes, 239.03 seconds).
TLS-only, TCP-only, PostgreSQL-only and all-component packaging passed on both
platforms. These are correctness gates, not benchmark timings or a security audit.

## Local Validation, 2026-10-07

Windows MSVC 14.44 with project-local OpenSSL 3.6.5 (vcpkg commit
`e182cb4dd2df2ab02f66a1aabd5f35bbdc9522c7`), and WSL2 Ubuntu 26.04 with GCC 15.2
and distribution OpenSSL `3.5.5-1ubuntu3.7`, passed Debug, Release and ASan
correctness suites (29 CTest entries per configuration). TLS ownership/session
hardening was then rebuilt and its affected TLS and component-packaging gates
rerun. The final TLS suite passes in all six configurations.

ASan supplements passed 20 Linux and 10 Windows repetitions of the TLS suite,
including both schedulers and supported four-worker I/O layouts. Both platforms
also passed 20/10 repetitions respectively of independent adapter interoperability
and the concurrent echo example. Additional session invalidation and corrupted
authenticated-record regressions pass on all six configurations. No TLS timing
measurements or performance claims are made here.

The expiry probe exposed that server session-cache lifetime does not reliably
bound the client's native session, so captures now have an explicit local lifetime
cap. Review also added fatal-error/unclean-close capture invalidation and a
resumption-time invalidation check. These are attributed, covered corrections,
not unexplained test failures or silently omitted evidence.

## Local Socket Requalification

Extracting the unchanged native socket awaiters into IO for the new Local
transport passed final Windows/Linux Debug, Release and ASan correctness suites,
including TLS and PostgreSQL interoperability. Local/TCP/combined packaging
also passed on both platforms. These are compatibility gates, not performance
measurements or new production/security certification.

An earlier Windows Debug suite had one failure at the five-second Task wrapper
in `Lazy TLS factories own credentials before execution starts`. The old assertion
did not record its error code; its exact cause is not established. It remains in
the ignored `local-sockets-qualification-20261007.json` diagnostic record.

The fixture listened only on IPv4 while connecting to `localhost`. Timed controls
resolved `::1` first, recorded a 2,040 ms native IPv6 refusal, and consistently
accepted IPv4 after roughly two seconds. Twenty original instrumented exchanges
and twenty timed original exchanges passed, so those controls did not reproduce
the original failure. A dual-stack control passed twenty exchanges with accepts
in 1-5 ms. The lifetime fixture now serves both families without increasing the
deadline, and reports the error message/code on failure. This removes a confirmed
unrelated refusal delay; it is not proof of the original failure's cause or fix.

## Passphrase Cleanup Qualification

Allocation probes exposed uncleansed owned passphrases on early client/server
factory validation failure. Factories now acquire cleanup before validation and
provider setup, preserving the existing identity-loader and credential-destructor
cleanup. Retained inline move sources are also cleared on the qualified toolchains.
No verification, session, transport or cancellation policy changed.

Isolated regression executables observe live passphrase allocations before delete
for validation, policy and missing-identity rejection. They link test-only allocator
observation code, never production hooks. Windows and Linux Debug/Release/ASan
TLS selections pass three and four entries respectively. The ASan supplements
pass 10 Windows repetitions (30 entry passes, 219.83 seconds) and 20 Linux
repetitions (80 entry passes, 72.88 seconds). PostgreSQL interoperability, real-server
multicore gates, TLS-only packaging and relocated consumers also pass on frozen
source. Raw evidence is in ignored credential qualification records.

This is best-effort cleanup of owned option storage, not secure allocation,
erasure of caller copies or every compiler/OpenSSL temporary, or an independent
security audit. The deliberate limits below still apply.

## Duplex Backpressure Correction

A controlled transport exposed a deadlock in both TLS 1.2 and TLS 1.3: the
writer held the send gate waiting for transport progress, while a reader with
no outgoing records joined that gate instead of consuming the peer's readable
application data. Reads now bypass the send gate only when their output BIO is
empty. Actual protocol output is still flushed; OpenSSL can generate writes
from a read operation, as its [SSL_read documentation](https://docs.openssl.org/3.5/man3/SSL_read/)
explains.

Writes, handshakes and shutdown retain their completion barrier even with an
empty BIO: another flush may already have removed their records and still be
writing them. No transport write buffers or active frames are released early.
The fix preserves cancellation poisoning, authenticated EOF and engine locking.

The original controlled probe timed out with both TLS versions. The correction
passed Windows ASan and Linux controls, including 20 process repetitions each,
before adding the permanent regression. Fresh frozen source then passed all
34 PostgreSQL/TLS supplemental gates on 2026-10-08, including six toolchain
configurations, four-worker real-server workloads, packaging and 10 Windows/
20 Linux TLS ASan repetitions (30/80 CTest entry passes respectively).

A separate earlier Windows ASan PostgreSQL run timed out in a client's pipeline
phase. Its failure is retained; the controlled TLS deadlock is confirmed, but
the earlier run did not capture enough state to establish that it was the same
failure. Passing controls and the fresh gate do not retroactively prove that
attribution. These are correctness checks, not performance measurements or
an independent security audit.

## Owning Metadata Qualification

Synchronous negotiated TLS snapshots and owning authenticated peer DER exports
are qualified on Windows/Linux Debug, Release and ASan, plus reduced-module
builds and relocated consumers. Native engine tests cover both TLS versions,
all client-auth policies, certificate presence, copy/move lifetime, ALPN and
actual stateful/ticket resumption with required mTLS. Four-worker PostgreSQL
controls cover the same snapshots over real verified TLS sessions.

The broader promotion passes 399 CTest entries; a subsequent test-only fixture
storage-duration correction passes its exact one-test selection in nine profiles.
The correction ensures fixture destruction on assertion exit as well as normal
return; it changes no library source. [Exact scope and sealed evidence](postgres-qualification.md#metadata-regression-qualification-2026-10-08).
No benchmark or new independent security/deployment certification is implied.

## Client SNI Policy Checkpoint (2026-10-10)

Per-handshake `server_name_indication` controls only the client's DNS routing
extension; certificate and DNS/IP verification remain mandatory. Session offers
bind the policy and reject mismatches before claiming a one-shot capture. Shared
credentials are not mutated. This does not implement server-side dynamic
certificate selection.

Permanent Windows Debug, Release, ASan and without-Runtime selections pass
60 TLS/PostgreSQL regression entries plus two relocated component-package entries.
The new engine suite observes actual ClientHello extensions, both TLS versions,
DNS/IPv4/IPv6 names, verification rejection, stateful/ticket resumption and
concurrent shared credentials. Independent PostgreSQL peers cover reset,
cancellation snapshots, pinned custom-provider OAuth and zero fallback accepts.
Native controls require matching OpenSSL headers/runtime and the pinned libpq
version; native dependencies are not sanitizer-instrumented baselines.

Linux Debug/Release development selections pass. The Linux ASan build was
interrupted by host disk exhaustion and an offline WSL disk with ext4 journal
errors, before its tests ran. Remaining Linux profiles/package gates, real-server
SNI controls and public resumed-stream adapter controls were not qualified by
that checkpoint's Windows passes. The subsequent adapter milestone below closes
the Windows stream gap only. [Original sealed evidence and retained failures](postgres-qualification.md#sni-policy-checkpoint-2026-10-10).

## SNI Stream Adapter Regressions (2026-10-10)

Permanent `weave_tls_sni_streams` exercises the public ordinary/resumed client
factories over native IOCP TCP, against an independent OpenSSL server engine.
Both TLS versions and stateful/ticket policies cover DNS and IPv4/IPv6 verification
names, SNI enabled/disabled, required ALPN, mTLS, observed resumption and graceful
shutdown. IPv6 here is a certificate-verification name over numeric IPv4 TCP,
not a claim of IPv6 transport coverage from this test.

Dropped unstarted tasks, changed SNI/name/ALPN/credential offers and duplicate
one-shot offers expose no TLS bytes. Mismatches preserve the shared capture for
a subsequent matching handshake. Wrong DNS/IP names and untrusted chains still
fail with certificate-verification errors under both SNI policies. The peer
observes ClientHello extension presence and actual native resumption, rather than
trusting configured intent or only the client's metadata.

Windows Debug, Release, ASan and without Runtime pass 48 TLS/PostgreSQL regression
entries plus two relocated package entries. Full profiles exercise four-worker
runtimes, both schedulers and sharded/shared layouts with eight concurrent chains
sharing immutable credentials. Twenty further ASan process repetitions pass
without retries. Owned fixture cleanup and exact aggregate audits are checked.
The test is promoted only after its exploratory implementation passed the same
four development profiles; no production library source changes.

Linux adapter qualification and Linux packages remain pending WSL recovery.
Windows leak checking is disabled and OpenSSL/native baselines are not fully
instrumented; this is not independent security review or a deployment soak.
[Exact scope and sealed evidence](postgres-qualification.md#sni-stream-adapter-regressions-2026-10-10).

## Client Certificate Policy Development (2026-10-10)

Per-handshake disable/allow/require now controls the client's identity without
mutating shared credentials. Require observes OpenSSL's CertificateRequest and
outgoing CertificateVerify messages; loading an unsuitable certificate alone is
not success. Session offers bind the mode before consuming their one-shot claim.
Required-mode resumption inherits the original confirmed identity use; a declined
offer must satisfy the policy through the resulting full handshake.

Windows Debug, Release, ASan and without-Runtime development selections pass
32 entries; migrated permanent option-schema/native expectations pass eight
more. Native engine controls, public resumed streams and independent PostgreSQL
TLS peers cover selection, ignored file/provider loading, startup/reset,
cancellation snapshots, pinned OAuth and terminal failure before application
Startup. A reproduced plaintext-policy omission was fixed; native libpq also
rejects require on plaintext, but at its authentication boundary.

This is not the broader TLS release gate or an independent security audit.
Permanent policy regression promotion, broader regression/package and real-server
controls, Linux qualification and longer adversarial/lifetime soaks remain.
Windows leak detection is disabled, and native dependencies are not fully
sanitizer-instrumented. The Python fixture uses OpenSSL 3.0.16 only as a functional
test peer, never a supported production provider; Weave/native clients use 3.6.5.
[Complete evidence and retained failures](postgres-qualification.md#client-certificate-policy-development-2026-10-10).

## Client Certificate Policy Regressions (2026-10-10)

The confirmed implementation now has permanent engine and public resumed-stream
tests owned by TLS, and parser/loader/startup/reset/cancellation/OAuth tests owned
by PostgreSQL. TLS tests do not depend on PostgreSQL; Runtime remains optional.
Independent native libpq controls stay behind WEAVE_POSTGRES_TLS_LIBPQ_TESTS.

Windows Debug, Release, ASan and without-Runtime profiles pass 36 focused policy
entries and 324 qualified broader TLS/PostgreSQL entries. Four configuration-driver
confirmations and two Release component packaging entries bring the total to
366. The original four silent configuration entries are retained but excluded:
the corrected driver captures five child selections and validates their positive
case/assertion counts before reporting success. No library code changes during
this promotion, and primary scratch sources are archived then removed.

The ASan profile instruments Weave and its linked tests, not the independent
native libpq control executables or all external dependencies. Windows leak
checking remains disabled. Real-server policy, local/GSS-specific and Linux
controls, sustained adversarial/lifetime soaks and wider release review remain
separate gates. No performance measurement or independent security audit is
claimed. [Exact inventory and immutable evidence](postgres-qualification.md#client-certificate-policy-regressions-2026-10-10).

## PostgreSQL Certificate Server Controls (2026-10-10)

Windows Debug, Release and ASan development controls now validate the per-client
certificate policy against actual PostgreSQL 18.6, including both TLS versions,
both negotiation modes, mTLS/SCRAM-PLUS, reset and observed SQL cancellation.
The resulting fix is confined to PostgreSQL's one-shot cancellation-response
read: Windows reset can mean the server's expected EOF after a complete request.
TLS engine and ordinary authenticated-EOF requirements are unchanged. Permanent
cancellation regressions pass four full/reduced Windows profiles; real-server
promotion, independent native controls and broader post-fix release gates remain.
[Exact scope and retained failures](postgres-qualification.md#windows-certificate-policy-server-controls-2026-10-10).

## Windows Cancellation Post-Fix Qualification (2026-10-10)

The complete registered TLS/PostgreSQL selection is rebuilt and passes after
the Windows cancellation fix: 82 entries each in Debug, Release, ASan and without
Runtime, plus two relocated Release package consumers. All 330 entries have
complete captured logs and exact-inventory checks. Truncated JUnit payloads are
retained as pass/fail inventories, not mistaken for complete assertion evidence.
Certificate-policy matrices, configuration assertions and cancellation cases
are separately checked against the full logs. No further production changes.

This closes the broader Windows post-fix regression/package gate only. Native
real-server certificate controls, final real-server promotion, local/GSS-specific
policy, Linux and sustained adversarial/lifetime qualification remain. Windows
leak detection is disabled; external dependencies are not fully instrumented.
[Exact evidence and limits](postgres-qualification.md#windows-cancellation-post-fix-qualification-2026-10-10).

## Deliberate Limits

Verification is secure by default; weaker client policies are explicit and
documented in [TLS](tls.md). There is no automatic network revocation fetching, Windows
enterprise-chain-policy equivalence, early data, DTLS, renegotiation, external PSK,
post-handshake client authentication, dynamic SNI certificate callback, or shared
ticket-key service is exposed. Multi-host certificate selection can use separate
listeners/credential snapshots until a separately reviewed SNI API exists.

OCSP clients reject session-resumption configuration, since resumed handshakes do
not necessarily supply fresh stapled evidence. Stateless ticket revocation requires
credential/key rotation, not cache clearing. Existing connections retain their old
policy snapshot and need application-directed cancellation if immediate revocation
is required.

Security-sensitive changes must rerun the relevant gates. Automated evidence does
not replace independent security review, deployment soak testing or incident
response. Do not describe these tests as a security audit.
