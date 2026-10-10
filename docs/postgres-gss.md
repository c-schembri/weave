# PostgreSQL GSS Authentication And Encryption

GSS authentication is opt-in. At a synchronous setup boundary, create a provider:

```cpp
auto gss = weave::pg::GssContext::create({.workers = 2, .capacity = 64});
if (!gss)
  return weave::report_error(gss.error());
```

Pass a copy into the coroutine's owning state:

```cpp
weave::Task<weave::pg::Connection> login(weave::pg::GssContext provider)
{
  weave::pg::Options options{
    .host = "database.example",
    .user = "application",
    .database = "application",
  };
  options.gss = std::move(provider);
  options.authentication.methods = {weave::pg::Authentication::gss};
  co_return co_await weave::pg::connect(options);
}
```

Create the provider at a synchronous setup boundary, then use it inside Tasks
or with `BlockingConnection`. Copies share one bounded provider pool; they do
not create more threads. Ordinary PostgreSQL connections create no provider
threads. `workers` counts credential-provider threads, not I/O/runtime workers.
`capacity` limits simultaneous native security contexts, including authentication,
live encrypted connections and queued retirement. Authentication-only sessions
release their slot after startup; encrypted connections retain it until explicit
asynchronous cleanup or provider retirement. Admission beyond that bound fails
with `std::errc::no_buffer_space`. Leave additional capacity for cancellation
connections and overlapping teardown.

## Identity And Policy

On Linux, `GssContextOptions::credential_cache` selects an MIT Kerberos cache,
for example `FILE:/run/application/krb5cc`. An empty value captures the system
default cache name at factory time. Later changes to `KRB5CCNAME` do not retarget
that provider. This captures a credential **source**, not immutable ticket
contents: renewals and changes to the selected cache can affect later logins.
Weave does not inherit a caller's thread-local `gss_krb5_ccache_name` override;
provide the cache explicitly when identity selection matters.

Windows captures and duplicates the factory caller's effective token. If the
caller is impersonating, provider workers use that identity even after the
caller reverts or closes the original token. Identification/anonymous tokens
are rejected. A failed token capture or impersonation never falls back to the
process identity. Windows does not accept a Kerberos cache path.

The default service is `postgres`; change `Options::gss_service` for another
service principal. The selected host name, not a pinned numeric transport
address, determines the target. Local-socket connections target `localhost`.
Delegation is off by default; `Options::gss_delegation` explicitly requests it.
A request is not a guarantee that the server receives delegated credentials.

`Options::gss_mutual` defaults to true. On Windows, mutual authentication
requires confirmed Kerberos, not an NTLM provider flag. Setting it false permits
non-mutual authentication only over verified TLS. Plaintext always requires
mutual authentication. GSS authentication itself does **not** encrypt subsequent
PostgreSQL traffic; use TLS or consciously opt into plaintext with mutual GSS.

Windows handles AuthenticationGSS with Kerberos and AuthenticationSSPI with
Negotiate. Linux uses Kerberos for either native challenge. Inclusive
`require_auth=gss`/`sspi` policies require a provider context before connecting;
they never silently enable ambient credentials. `channel_binding=require`
continues to require SCRAM-PLUS and rejects native GSS before sending a proof.

The parser/explicit configuration loader support `krbsrvname`, native `gsslib`,
and `gssdelegation=0|1`, but never create a provider pool. Attach `Options::gss`
after parsing/loading. `gsslib=sspi` is Windows-only; `gsslib=gssapi` is Linux-only.
The parser also accepts `gssencmode=disable|prefer|require`; the default is
`disable`. Parsing/loading never discovers credentials or creates workers.

## Encrypted Connections

Attach an explicit provider and select the transport policy:

```cpp
options.gss = provider;
options.gss_encryption = weave::pg::GssEncryption::require;
options.authentication.methods = {weave::pg::Authentication::gss};
auto connection = co_await weave::pg::connect(options);
```

`Connection::gss_encrypted()` and `BlockingConnection::gss_encrypted()` are
synchronous observations of the selected transport, not promises that a closed
connection is still open. GSS encryption takes priority over configured TLS.
It always requires mutually authenticated Kerberos and encrypted records,
regardless of the authentication-only `gss_mutual` option.

`require` rejects a server that declines GSS encryption. `prefer` falls back
**only** after the server's explicit `N` response: it then uses verified TLS,
or plaintext if `Options::plaintext` was explicitly enabled. Native proof,
socket, framing and protected-record failures never retry in plaintext/TLS or
on another host. Encrypted cancellation snapshots always require GSS encryption,
even if the original connection used `prefer`.
With encryption enabled, any timeout after TCP connection is also terminal rather
than advancing to a potentially weaker host. Pre-connect failures can still try
another configured destination; explicit target-session rejection can select the
next eligible server.

Weave reads exactly one negotiation-response byte and never exposes an
unauthenticated server ErrorResponse as a diagnostic. A successful encrypted
transport can satisfy `require_auth=gss` without an additional PostgreSQL GSS
challenge, following libpq's implicit-authentication rule. Other explicit
authentication challenges still obey the selected policy before credentials
are sent. `channel_binding=require` requires TLS-backed SCRAM-PLUS: a server's
positive GSS-encryption response is rejected before sending a native proof.

Deliberate libpq differences: GSS encryption is disabled by default, providers
are explicit, `prefer` never falls back after a failed negotiation, and local
sockets reject non-disabled encryption modes rather than silently ignoring them.
Cleartext password authentication still needs its own explicit opt-in and an
actually verified TLS or mutually authenticated encrypted GSS transport.

## Protected Record Lifetime

Protected contexts require Kerberos mutual authentication, confidentiality,
integrity, replay detection and sequence detection. Linux uses native
`gss_wrap_size_limit`; Windows uses native SSPI trailer/padding bounds. Records
are bounded to PostgreSQL's 16 KiB packet limit, including the four-byte wire
length. Tampered, replayed, out-of-sequence and integrity-only input is terminal;
there is no hand-written cryptography or fallback to unsigned/plain traffic.
GSS has no authenticated TLS-style `close_notify`. TCP EOF is accepted only
between complete protected records; truncation inside a record is an error.
This does not authenticate orderly end-of-stream against a transport attacker.

Native operations serialize per session on provider workers, not I/O threads.
Diagnostic and metadata observations are owning, synchronized snapshots, not
borrowed views into state another provider call can mutate.
Explicit asynchronous cleanup still drains shielded destruction. An idle
session's synchronous destruction transfers its preallocated owning record to
the provider queue instead of destroying native state locally. Such retirement
has reserved admission and no Context/executor callback. All session borrowers
must already have drained; retirement never makes active-frame destruction safe.
`Connection::finish()` and `reset()` drain native destruction before recycling
the slot, including reset after terminal failure or synchronous `close()`.
Synchronous close only closes the socket; the owned native context remains until
finish, reset or destruction. This distinction also applies to capacity planning.

Provider workers do not retain owning pool references. The final `GssContext`
owner joins and drains any transferred destruction before releasing the captured
identity. That final synchronous teardown can wait for native cleanup; it is not
a hard-deadline or nonblocking-destructor promise. Keep an explicit provider
owner alive through the application's asynchronous cleanup boundary.

Linux encrypted connections are tested against an owned PostgreSQL/Kerberos
deployment and a libpq encryption control. An independent MIT acceptor supplies
hostile protected traffic. Windows SSPI protection and transport are implemented
and build-checked, but positive domain/Kerberos encryption remains unqualified here.

## Cancellation And Qualification

Native calls can contact a KDC and cannot be safely preempted. They run on the
explicit provider pool, never on an I/O worker. Cancellation prevents new native
work where possible and drains in-flight calls before the operation returns.
Startup failure also drains shielded native destruction. Cancellation after a
protected transfer starts makes the connection terminal; its native context
remains owned until finish, reset or destruction, as described above.
Consequently a connection deadline is **not** a hard upper
bound on credential-provider latency. Configure native Kerberos timeouts and
deployments accordingly. Cleanup has reserved admission even when the pool is full.

Completion routing retains the coroutine frame and original Context/executor
until the foreign provider thread leaves publication. Both Context and Runtime
use the same startup implementation; the blocking facade drives that engine too.

Linux qualification uses an owned disposable KDC and PostgreSQL cluster, with a
libpq fixture controls, large queries/parameters/results, batching, COPY, reset,
owning encrypted cancellation, hostile records, saturation and delayed native
initiation/wrap/unwrap cancellation. Both four-worker schedulers and the blocking
facade use the same transport. Enable both `WEAVE_POSTGRES_KERBEROS_TESTS=ON`
and `WEAVE_POSTGRES_SERVER_TESTS=ON`, and supply `WEAVE_POSTGRES_SERVER_BIN`.
The delayed-native tests currently require GCC's shared ASan runtime for ASan
qualification. Native SSPI startup qualification is separately opt-in on Windows
with `WEAVE_POSTGRES_SSPI_TESTS=ON`; it exercises a local native acceptor over
verified TLS and effective-token capture.

Enable `WEAVE_POSTGRES_OAUTH_SERVER_TESTS=ON` as well, with PostgreSQL 18 server
headers in `WEAVE_POSTGRES_OAUTH_SERVER_INCLUDE`, to add
`weave_postgres_oauth_gss`. That test combines native encryption with OAuth cache
hits and custom/device acquisition. Backend SQL verifies GSS encryption, no TLS,
and no substitution of GSS authentication for OAuth. An independent native peer
declines encryption on the discovery reconnect: no startup/token bytes or host
failover may follow. Single-slot reset, cancellation and shutdown controls check
that discovery and provider failures release native admission.
The libpq functional control runs separately; its system-library memory safety
is not qualified by Weave's ASan process.

Positive Windows domain/Kerberos login remains unqualified on this non-domain
machine. These tests establish the documented Linux integration, not Windows
domain interoperability, full libpq parity, or an independent security audit.
See [qualification evidence](postgres-qualification.md#native-gss-encrypted-transport).
