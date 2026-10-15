# TLS

Include `<weave/tls.hpp>` and link `weave::tls`. OpenSSL 3.5+ supplies TLS and
cryptography over Weave streams; TCP/Runtime-only consumers do not link it.
See [TLS examples](examples/echo/README.md) and
[validation/deployment boundaries](../../development/testing/README.md).

## Client

```cpp
static weave::Task<void> connect(weave::TlsContext credentials)
{
  auto client = co_await weave::tls::connect(credentials, "example.com", 443);
  co_await client.shutdown();
}
```

Create credentials synchronously with `TlsContext::client()`, then run a
Task with Context/Runtime. Client defaults verify chain and DNS name/IP SAN;
DNS names send SNI, IP literals do not. Explicit certificate verification omits
the hostname check; none encrypts without authenticating the server.
Neither replaces the secure default. Metadata reports checks actually performed;
unverified certificates are not exposed as authenticated peer identities.

An explicit CA file/directory replaces default trust policy. Windows additionally
imports ROOT certificates; this is **not** complete Windows chain policy,
enterprise/distrust handling or automatic revocation checking.
Verification-none rejects revocation enforcement and session resumption.

TLS 1.2/1.3 are supported; older versions, compression, renegotiation and early
data are disabled. Default TLS 1.2 uses ECDHE with AES-GCM/ChaCha20.
`TlsCipherPolicy` can set suites/groups/signatures; security levels below 2 fail.

ALPN labels are bounded binary values, at most 255 bytes each.
Context `alpn` controls available protocols; per-handshake `required_protocol`
offers only that label on clients and rejects missing/different selection in
either role. ALPN is a label, not an HTTP implementation.
Per-handshake SNI disablement changes routing, never verification.

## Server And mTLS

```cpp
auto credentials = weave::TlsContext::server({
  .certificate_file = "server.pem",
  .private_key_file = "server-key.pem",
  .client_auth = weave::TlsClientAuth::required,
  .ca_file = "client-ca.pem",
});
```

Server client-authentication policy is none, optional or required.
Provide trusted client issuers and revocation policy explicitly.
Clients supply certificate/key credentials; per-handshake
`TlsCertificateMode::disable/allow/require` controls presentation.
Require fails unless requested identity was actually used with proof of
possession. It does not prove application authorization. Resumed mTLS inherits
its validated identity under the configured session policy.

`tls::connect` establishes TCP and TLS. `tls::client` / `tls::server` upgrade
an owned compatible stream, including an accepted TCP connection.
Credential contexts are immutable, cheaply copied snapshots. Rotate by constructing
a new context; existing streams retain the old snapshot. No mutable SSL_CTX escape
or implicit hot reload is exposed.

## Credentials

Factories load files synchronously. Prebuild credentials before worker execution
when disk or provider latency matters; cooperative deadlines cannot preempt that work.
Client/server keys support explicit PEM, DER or an already-configured OpenSSL STORE
locator. STORE format is not inferred, installs no hardware provider, and does not
provide legacy ENGINE compatibility.

An owning `TlsPasswordProvider` supplies a synchronous
`Result<std::string>(std::string_view) const noexcept` callback.
The path/URI is borrowed for the invocation; synchronize mutable captured state.
Password and provider cannot both be supplied. Empty/moved providers, oversized
passwords and provider failures fail setup; no terminal prompt or fallback occurs.
Embedded NUL is handled by length, not truncation. Completed TLS credentials do not
retain the provider. Owned temporary secrets are best-effort cleansed, not locked
memory or a guarantee covering caller copies/OpenSSL/compiler temporaries.

## I/O And Lifetime

`read`, `read_exactly`, `write_all`, `shutdown_send` and `shutdown` are
Tasks. TLS shutdown exchanges protocol messages; unlike TCP shutdown it is not
a synchronous socket setter. A nonempty read returns zero only after authenticated
close_notify. Abrupt peer closure is `TlsError::truncated`; an empty read is not
evidence of EOF.

One reader and one writer may overlap with serialized TLS-engine access.
Competing readers/writers and shutdown during active I/O fail operation_in_progress.
Borrowed buffers and streams must outlive their operations.
Each stream lazily retains a 17 KiB send buffer and a 16 KiB receive buffer;
the directional gates protect reuse, and idle moves transfer their ownership.
Once an operation advances TLS state, cancellation retires the engine with a sticky
failure after native I/O drains; a pre-canceled unstarted operation does not corrupt
an otherwise usable stream. No hidden close_notify is sent by the destructor.

Handshake timeout defaults to 30 seconds and must be positive, at most 24 hours.
It is cooperative and includes cleanup. Synchronous `close()` refuses active
operations; drain all Tasks before stream destruction. These are ownership
contracts, not thread-safe concurrent session mutation.

`TlsInfo` owns negotiated metadata and verified identity.
`TlsLimits` defaults to 256 KiB input/output, 64 KiB certificate chain and
verification depth 16. These bound retained engine data, not process RSS or the
number of accepted clients; application admission remains necessary.

## Revocation

CRL policy checks leaf or chain using a file/trusted OpenSSL hashed directory.
Required missing, stale or invalid CRLs fail the handshake. Directory lookup can
be lazy; successful factory construction does not prove every chain's CRL exists.
There is no network fetch or hot reload.

OCSP if_present/required verifies every received response: signature, issuer,
serial, GOOD status and freshness. Servers supply a matching fresh DER staple;
operators must refresh it. Clients cryptographically verify it.
Enforced client OCSP with session resumption is unsupported rather than silently
skipping a fresh check. Default trust alone is not revocation enforcement.

## Sessions

Resumption is disabled by default. Clients opt in; servers choose disabled,
bounded stateful cache or tickets. Default session lifetime is 10 minutes, at
most 24 hours; ticket count is 1-16. TLS 1.3 stateful mode still sends tickets
backed by server cache state.

Captured client `TlsSession` is move-only and one-shot. It binds credential
snapshot, peer name, ALPN, SNI and client-certificate policy, with expiry bounded
by configured lifetime and certificate validity. A newly captured ticket replaces
older capture state; tickets may arrive only after application reads.
Check availability, and inspect `session_reused()` rather than assuming resumption.

Fatal I/O, cancellation and destruction without completed close_notify invalidate
captured client sessions; graceful shutdown preserves eligible ones.
Resumed identities are revalidated. `clear_sessions()` clears server stateful
cache, not issued stateless tickets; it is not universal revocation.
Rotate credentials/ticket context when full ticket invalidation is required.

## Exporters

Bounded TLS exporters expose application key material with explicit label/context.
Absent and empty context are distinct; reserved labels are rejected.
Treat outputs as secrets. `tls-server-end-point` channel binding uses the peer
certificate under RFC 5929, not a generic authentication token.

## Sensitive Key Logging

Explicit `key_log_file` writes NSS/Wireshark traffic secrets for TLS 1.2/1.3.
Possession can decrypt captures. Default is off; `SSLKEYLOGFILE` does not enable
it. Do not put it in ordinary production credentials.

Use a trusted local directory. New files are owner-private on Linux and have
current-user/SYSTEM protected ACLs on Windows. Existing files must be regular,
private and owner-controlled; leaf symlinks/reparse points, unsafe modes and alien
ACL grants are rejected. Parent-directory traversal, subsequent ACL changes and
administrator access remain deployment responsibilities.

Records append without truncation. Opening/appending is synchronous and may block
a worker. Setup failure fails the credential factory; write failure latches and
fails TLS operations, never silently disables logging.
The sink lives with credentials and streams; close those owners before removing
the Windows file. Metadata exposes enabled state, not the sensitive path or records.
