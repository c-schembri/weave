# TLS

Link optional `weave::tls` and include `<weave/tls.hpp>`. It uses OpenSSL 3.5+
over ordinary asynchronous stream
operations; Weave does not implement TLS parsing or cryptography. TCP-only and
Runtime-only consumers do not discover or link OpenSSL.

## Client

```cpp
#include <weave/tls.hpp>
#include <weave/log.hpp>

static weave::Task<void> connect(const weave::TlsContext &credentials)
{
  auto client = co_await weave::tls::connect(credentials, "example.com", 443);
  // Use read(), read_exactly(), and write_all() as with TCP.
  co_await client.shutdown();
}

int main()
{
  auto credentials = weave::TlsContext::client();
  if (!credentials)
    return weave::report_error(credentials.error());

  auto ctx = weave::Context::create();
  if (!ctx)
    return weave::report_error(ctx.error());

  auto result = ctx->run(connect(*credentials));
  if (!result)
    return weave::report_error(result.error());
}
```

Client verification defaults to `TlsVerification::hostname`: DNS names verify
certificate hostnames and send SNI; IP literals verify IP subject-alternative names.
Explicit `certificate` verifies the chain without checking the name; `none`
encrypts without authenticating the server. Neither is a secure substitute for
the default. `TlsContext::client({.ca_file = "issuer.pem"})` uses an
explicit trust file instead of defaults. OpenSSL default paths are loaded on both
platforms; Windows additionally imports ROOT certificates. Importing ROOT is not equivalent to the complete
Windows certificate-chain policy: distrust, revocation and enterprise policy are
not mirrored. Use explicit CA, CRL and OCSP policy when your deployment needs
revocation checks. Root import is not a claim of Windows chain-policy equivalence.

`TlsInfo::certificate_verified` and `hostname_verified` report checks actually
performed, independently of whether TLS encrypted the transport. Unverified
certificates are not returned by `peer_identity()` or `TlsInfo::peer`.
Verification-none rejects configured CRL/OCSP enforcement and session resumption;
certificate-only sessions still bind credentials and the supplied name. Server
client-authentication policy is unchanged.

### Sensitive Key Logging

Client and server credential options accept an explicit `key_log_file` path;
PostgreSQL accepts the `sslkeylogfile` keyword. This writes native NSS/Wireshark
traffic-key records, including TLS 1.2 and TLS 1.3 secrets. Possession of the file
can decrypt captured traffic: protect, retain and delete it like private keys.
It is disabled by default and never enabled by `SSLKEYLOGFILE` or another
environment variable. Do not enable it in ordinary production credentials.

Use a trusted local directory. New files are owner-private on Linux and use a
protected current-user/SYSTEM DACL on Windows. Existing files must be regular,
owner-controlled and private; symbolic/reparse leaf paths, unsafe permissions
and unrelated Windows ACL grants are rejected. Directory traversal, later ACL
changes and administrator access remain deployment responsibilities.

Records append without truncating an existing log. Shared contexts serialize
writes; independent contexts use native append on a local filesystem. File
opening and logging are synchronous and can block an execution worker. Setup
errors fail the credential factory; write errors latch and fail subsequent TLS
engine operations instead of silently disabling logging. The open sink lives
with the credential snapshot and its streams; close those owners before removing
the log on Windows. Configuration snapshots expose only an enabled flag, never
the sensitive path or records.

TLS 1.2 and 1.3 are enabled; older protocols are unavailable. Set `min_version`
and `max_version` to constrain negotiation, and `.alpn = {"h2", "http/1.1"}`
to offer application protocols. Server preference determines selection when both
sides offer ALPN; incompatible offers fail. ALPN negotiates a label, not an HTTP
implementation. `version()`, `cipher()` and `negotiated_protocol()` are synchronous
queries. The default TLS 1.2 cipher policy requires ECDHE with AES-GCM or ChaCha20;
TLS 1.3 uses OpenSSL's default suites. Compression, renegotiation and early data
are disabled. `TlsCipherPolicy` configures suites, groups, signature algorithms
and security levels; levels below 2 are rejected.

## Server And Adapters

```cpp
auto credentials = weave::TlsContext::server({
  .certificate_file = "chain.pem",
  .private_key_file = "key.pem"
});
```

The synchronous factory validates the PEM chain and matching key. Encrypted keys
can use `private_key_password` or an owning `private_key_password_provider`;
loading never prompts on stdin. The credential object is cheaply copyable and
shareable across workers.

`private_key_format` selects `TlsPrivateKeyFormat::pem` (default), `der`, or
`store`. DER loads an ASN.1 private-key file; STORE treats `private_key_file` as
an OpenSSL STORE locator, such as a `file:` URI. STORE must produce exactly one
private key and the key must match the certificate. It uses already configured
OpenSSL providers, not a legacy ENGINE API or implicit provider installation.
Password callbacks are demand-driven and never fall back to stdin. URI credentials
may contain secrets: PostgreSQL configuration snapshots redact STORE locators.
External hardware/provider deployments are not qualified by the file-STORE tests.

Factories cleanse their owned passphrase on validation, provider and identity
loading failure as well as successful loading. Internal moved-from inline storage
is also cleared on the qualified toolchains. This is best-effort cleanup of
owned buffers, not secure allocation or erasure of caller-owned copies and all
compiler/OpenSSL temporaries.

### Passphrase Providers

Both client and server options accept a checked, owning `TlsPasswordProvider`:

```cpp
// Application-supplied synchronous lookup; no exceptions or implicit prompting.
weave::Result<std::string> read_key_password(std::string_view file) noexcept;

auto provider = weave::TlsPasswordProvider::create(read_key_password);
if (!provider)
  return weave::report_error(provider.error());

auto credentials = weave::TlsContext::client({
  .ca_file = "issuer.pem",
  .certificate_file = "client.pem",
  .private_key_file = "client-key.pem",
  .private_key_password_provider = *provider
});
```

`Handler` is a move-only, const-invocable, noexcept callable returning
`Result<std::string>` from a borrowed key-file path. Provider copies share its
owning state, so mutable captured state must be synchronized by the application.
There is no hidden worker, global hook, or library-imposed callback lock.

OpenSSL requests the password only when decoding an encrypted PEM key; an
unencrypted key does not invoke the provider. Do not assume a particular number
of requests across native decoder attempts. The path is valid only through each
invocation. A callback error is preserved exactly, even with error-code value
zero; check the factory's Result, not just the truthiness of its error_code.
No default password, stdin prompt, truncation or retrying provider follows a
callback failure. Secrets must fit the native buffer with room for a terminating
NUL; embedded NUL bytes are preserved through the explicit password length.
The [OpenSSL PEM callback contract](https://docs.openssl.org/3.5/man3/PEM_read_bio_PrivateKey/)
defines the underlying byte-sequence and error-return behavior.

A nonempty explicit password and a provider conflict. Unused providers,
moved-from provider handles and NUL-containing paths are rejected. Other setup
failures, such as a mismatching key, can occur after a password was obtained.
Weave clears its returned password buffer on every such path and retains neither
provider nor passphrase in the completed TlsContext. Caller copies, callable
captures and compiler/OpenSSL temporaries remain outside that cleansing guarantee.

Construction and callbacks are synchronous and cannot be preempted by coroutine
deadlines. Preload credentials before starting workers when lookup/file I/O could
block; slow asynchronous secret acquisition belongs in application setup before
calling the credential factory. The provider implementation has six-profile
development and existing release-gate evidence, followed by permanent TLS
factory/stream regressions on nine full/reduced profiles. PostgreSQL startup,
reset, lazy ownership and terminal-failure regressions now pass on the same
profile matrix, alongside independent native libpq hook controls;
[qualification and limits](postgres-qualification.md#postgresql-key-password-provider-regressions-2026-10-10).

Each upgrade has a default 30-second handshake deadline. Override it with
`TlsHandshakeOptions`, for example `tls::server(transport, credentials,
{.timeout = 5s})`. Positive deadlines up to 24 hours are accepted. Timeout cancels
and drains transport I/O; it does not forcibly destroy an active coroutine.

An optional nonempty `TlsHandshakeOptions::required_protocol` requires an exact
ALPN selection before exposing the stream. Clients offer only that label; servers
still select using their immutable credential ALPN policy and reject a missing or
different selection. Empty preserves ordinary credential behavior. Labels are
binary values of at most 255 bytes, not necessarily UTF8. The override does not
mutate shared credentials, and captured sessions are bound to the same required
label; a mismatched offer is rejected before consuming its one-shot session.
This policy has permanent engine regressions and nine-profile existing TLS/package
gates. PostgreSQL synthetic startup/reset/cancellation/pinned-OAuth regressions
also pass nine profiles. Permanent real mTLS/SCRAM-PLUS/query-cancellation and
native libpq controls pass those profiles, with four Linux protected-GSS priority
profiles. Broader deployment/release qualification remains. [Exact scope](postgres-qualification.md#direct-tls-server-regression-qualification-2026-10-10).

Inside a handler, upgrade an owned transport:

```cpp
auto client = co_await weave::tls::server(std::move(transport), credentials);
```

`tls::client(transport, credentials, server_name)` performs the corresponding
verified client handshake. Both adapt any nothrow-movable `CancellableStream` and
return `TlsStream<S>` only after the handshake completes. Upgrade/connect tasks
own a cheap credential snapshot from construction, including before their lazy
execution starts; each established stream retains its native credentials.
No backend socket types or OpenSSL headers enter the public API.

Client handshakes default to sending Server Name Indication (SNI) for DNS names.
Set `TlsHandshakeOptions::server_name_indication = false` to omit that routing
extension. The supplied `server_name` still controls configured certificate and
DNS/IP verification; this is not an insecure verification switch. Numeric IP
names omit SNI regardless of the setting. The policy belongs to each handshake,
not the shared credentials, and does not affect server handshakes.
Captured sessions bind this policy as well as the verification name and required
ALPN label; a mismatch rejects the offer without consuming it.
[Current Windows/Linux SNI stream qualification](postgres-closure.md).

See the [concurrent TLS echo example](../modules/tls/examples/echo/README.md).

### Client Certificate Policy

`TlsHandshakeOptions::client_certificate` controls identity use on each client
handshake, independently of the server's `TlsClientAuth` policy:

- `TlsCertificateMode::disable` sends no client identity, even when credentials contain one.
- `allow` is the default: use an available compatible identity when requested.
- `require` rejects the handshake unless OpenSSL observes the server's request
  and constructs the client's signed CertificateVerify proof.

This does not prove that the server validates or authorizes the identity. Server
certificate/name verification follows the explicit credential policy. The identity policy is
per SSL object, never a mutation of shared credentials; server handshakes ignore it.

Sessions bind the mode before the one-shot claim. A matching required-mode
resumption inherits the original session's confirmed identity use; abbreviated
handshakes need not send a fresh certificate. If the server declines resumption,
the full handshake must satisfy the requirement again.

[Current platform regressions, native controls and component packaging](postgres-closure.md).

## I/O, Shutdown And Cancellation

One reader and one writer may overlap. Competing operations in the same direction,
or shutdown while I/O is active, return `operation_in_progress`. This does not
make a stream usable from arbitrary threads or unrelated Contexts. SSL engine
calls are serialized; underlying transport reads/writes retain their usual affinity.

With a nonempty buffer, `read` returns zero only for authenticated TLS `close_notify`.
An empty-buffer read is a no-op, not an EOF observation. Abrupt transport
EOF is `TlsError::truncated`, not successful EOF. `shutdown_send()` sends and
flushes `close_notify` without waiting for the peer. `shutdown()` also waits for
the peer notification. **Both are asynchronous**, unlike TCP's native half-close;
use `timeout` when an unresponsive peer must be bounded. Drain application reads
before shutdown if unread application data matters.

`cancel()` and `close()` are truly synchronous `Result<void>` APIs. Cancellation
once a TLS operation advances its encrypted state makes the stream terminal:
continuing after a partially sent record is unsafe. Subsequent operations return
the sticky error. A pre-cancelled task that never starts an operation does not
alter the TLS state. Cooperative cancellation drains waiters/native completion
before frames and borrowed buffers are reclaimed.

`close()` refuses active I/O and closes the transport without a graceful TLS
exchange. Destruction likewise never blocks or sends `close_notify`; it requires
all operations to have drained. Move a stream only while idle. Its Context and
borrowed buffers must remain alive through completion.

## Mutual TLS And Revocation

```cpp
auto server = weave::TlsContext::server({
  .certificate_file = "server-chain.pem",
  .private_key_file = "server-key.pem",
  .client_auth = weave::TlsClientAuth::required,
  .ca_file = "client-ca.pem"
});

auto client = weave::TlsContext::client({
  .ca_file = "server-ca.pem",
  .certificate_file = "client-chain.pem",
  .private_key_file = "client-key.pem"
});
```

`required` rejects absent or invalid client certificates. `optional` allows an
absent certificate, but rejects an invalid one. Both require an explicit client
CA file or hashed CA directory, not the public root store. `peer_identity()`
returns the verified certificate subject, issuer, SHA-256 fingerprint and owning
DER-encoded leaf certificate; it
fails when there is no authenticated peer certificate. Certificate verification
is not application authorization: the service must decide which identities may
perform which actions.

Synchronous `info()` returns `Result<TlsInfo>` with owning negotiated version,
cipher, symmetric key strength, compression, ALPN, actual resumption and optional
authenticated peer identity. It exports no native handles or session secrets.
Copies remain valid after stream destruction; they do not provide fresh
revocation or authorization evidence. [Metadata contracts](postgres-metadata.md#tls-fields).

Both client and mTLS server options accept `crl_file`, `crl_directory`, and
`revocation` (`leaf` or `chain`). Files contain PEM CRLs; directories use OpenSSL's
hashed layout (`issuer-hash.r0`, etc.). Both sources may be configured together.
Missing, stale or invalid required CRLs fail verification; `chain` needs CRLs for
each checked issuer. Directory lookup is lazy, so successful factory creation
does not prove that the required CRLs exist. The directory is trusted input:
OpenSSL's hash-directory lookup can also load certificates, not just CRLs.
There is no network fetching or policy hot reload. Replace credentials when the
policy changes. [OpenSSL directory contract](https://docs.openssl.org/3.6/man3/X509_LOOKUP_hash_dir/).

Clients can request stapled OCSP with `ocsp = TlsOcsp::if_present` or `required`.
Both validate any received staple's signature, issuer, certificate identifier,
GOOD status, thisUpdate age and nextUpdate. `required` also rejects absence.
`ocsp_max_age` and `ocsp_clock_skew` bound freshness tolerance. Servers accept a
DER `ocsp_file`, reject invalid/stale/non-GOOD staples at setup and check freshness
again before sending. Server setup checks matching serial and freshness; clients
perform the cryptographic verification. Provisioning and refreshing correctly
signed staples remains the server operator's responsibility.

## Sessions And Credential Rotation

Sessions are disabled by default. Enable client capture with
`.session_resumption = true`, and server sessions with
`.sessions = {.mode = TlsSessionMode::stateful}` or `tickets`.
Client `session_lifetime` independently caps captured sessions to 10 minutes by
default (configurable up to 24 hours). The server's cache lifetime is not a
reliable client-side expiry signal, particularly in TLS 1.2.
Server controls include bounded cache `capacity`, `lifetime` (up to 24 hours), and
ticket count (1-16). TLS 1.3 still uses tickets in stateful mode, but the server
cache must contain the referenced session. Early data/0-RTT remains disabled.

```cpp
auto session = client.session();
if (!session)
  co_await weave::fail(session.error());

auto next = co_await weave::tls::connect(credentials, "example.com", 443,
  std::move(*session));
```

TLS 1.3 tickets arrive after the handshake and are processed during reads. Until
then `session()` may report `session_unavailable`. The latest ticket is captured
as an immutable, in-memory snapshot; it is never serialized or logged. Captures
of the same ticket share a one-shot offer claim. Handles are bound to the exact
credential snapshot, server name, required ALPN label and SNI policy, and expire with the session or saved
certificate chain. Capturing a newer ticket invalidates the previous capture from
that stream. Fatal errors, cancellation and destruction without sending
`close_notify` also invalidate outstanding captures; graceful shutdown preserves
them. A resumed handshake revalidates certificate policy and time;
the server may refuse resumption and perform a full handshake instead.
`session_reused()` reports what actually happened. `session_stats()` provides
thread-safe cumulative verified-handshake and resumption counts.

`clear_sessions()` invalidates a server's stateful cache. It cannot revoke
stateless tickets and returns `operation_not_supported` in ticket mode. To rotate
certificates, trust, CRLs or ticket keys, create a new `TlsContext`, then publish
that immutable snapshot to new connection handlers using application-level
synchronization. Existing streams retain their original snapshot and finish
normally; old credentials must no longer be selected for new connections.
No live SSL_CTX policy mutation or cross-process ticket-key sharing is exposed.

Client OCSP enforcement cannot currently be combined with session resumption;
the factory rejects that combination rather than silently skipping fresh evidence.
Resumption is an authentication-policy decision, not just a performance switch.

## Bounds And Channel Binding

`TlsLimits` bounds encrypted input/output buffers (256 KiB each by default),
certificate-chain size (64 KiB) and verification depth (16). Exceeding a buffer
bound is terminal. These are engine limits, not a process-wide memory budget;
services still need connection admission limits and application I/O deadlines.

`channel_binding()` returns RFC 5929 `tls-server-end-point` bytes derived from the
server certificate, for protocols such as PostgreSQL SCRAM-SHA-256-PLUS. An
unsupported signature digest returns an error, not invented binding data.
`export_keying_material(label, size, optional_context)` provides OpenSSL's TLS
exporter, preserving absent-vs-empty context semantics. Reserved labels and
oversized requests are rejected. Never log session handles or exported keying
material.

## Build And Validation

Install a security-patched OpenSSL 3.5+ separately (for example
`vcpkg install openssl:x64-windows`), then:

```sh
cmake --preset windows-tls -DOPENSSL_ROOT_DIR=C:/Libraries/openssl
cmake --build --preset tls-release --parallel 4
ctest --preset tls-release
```

Use `asan-tls` for sanitizer validation. `WEAVE_MODULES=tls` selects TCP, Sync, IO
and Core automatically; Runtime remains optional. Installed consumers use
`find_package(weave CONFIG REQUIRED COMPONENTS tls)`. Shared OpenSSL deployments
must provide its runtime DLLs; development targets copy DLLs from the detected
`bin` directory, overridable with `WEAVE_TLS_RUNTIME_DIR`. Installed consumers
remain responsible for their dependency deployment.

Linux presets include TLS and use system OpenSSL libraries and trust paths.
[Linux/WSL build instructions](linux.md).

Tests generate temporary credentials, CRLs and OCSP responses at execution, not
committed private keys. They exercise both TLS versions, names/IPs, rejected
chains/expiry/ALPN, mTLS roles, revocation, session policy and invalidation,
malformed records, buffer bounds, deadlines, fragmentation, full duplex,
truncation, cancellation, and four-worker configurations.
Python's independent TLS adapter checks both client/server roles on loopback.
Correctness CI runs these tests without benchmarks or external network peers.

See the [release gates and deployment responsibilities](tls-release.md). Tests are
not an independent security audit or a guarantee for every OpenSSL provider and
enterprise trust configuration. OpenSSL 3.0 reached
[upstream end-of-life](https://openssl-library.org/post/2026-09-16-eol30/);
production deployments must track security updates and support lifetimes.

Engine rules follow OpenSSL's
[error handling](https://docs.openssl.org/3.6/man3/SSL_get_error/),
[write retries](https://docs.openssl.org/3.6/man3/SSL_write/), and
[shutdown](https://docs.openssl.org/3.6/man3/SSL_shutdown/) contracts.
