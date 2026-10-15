# PostgreSQL Connections

`pg::connect(Options)` owns configuration before its lazy Task begins. Network
startup uses the executing Context; no hidden runtime or networking thread is
created. Credential-file loading is synchronous. Prebuild `TlsContext` before
starting workers when file/provider latency matters.

## Configuration

```cpp
auto options = weave::pg::Options::parse(
  "postgresql://app@db.example/app?sslmode=verify-full&sslrootcert=ca.pem");
if (!options)
  co_await weave::fail(options.error());

options->password = password;
auto database = co_await weave::pg::connect(std::move(*options));
```

`parse` accepts keyword/value syntax with quotes/backslash escapes and
`postgresql://` / `postgres://` URIs. Percent decoding does not convert
`+` to a space. Duplicate settings use the last value; URI query settings
override authority/path fields. Invalid/unknown settings fail rather than being
ignored. Input is bounded to 1 MiB, decoded fields to 64 KiB and hosts to 64.

Parsing performs no I/O, environment loading or OS-account lookup. Defaults are
verified TLS, TCP localhost on **both platforms**, UTF8, application name
`weave`, a 30-second connect timeout, and protocol minimum 3.0 / maximum 3.2.
Supply a user; an omitted database uses it. Pin maximum 3.0 for servers that do
not negotiate 3.2. A downgrade below the minimum fails before credentials.

`option_schema()` is a synchronous immutable span describing recognized
keywords, environment mappings, defaults, secret markers and constraints.
It is the compact reference for the complete option vocabulary, not resolved
configuration or proof that every native libpq value/provider works.
`Options::info()` exposes a nonsecret owning configuration snapshot;
application/options text is still untrusted and must not be logged automatically.

### Explicit Loading

```cpp
auto options = weave::pg::Options::load("service=app", {
  .environment = false,
  .user_files = false,
  .service_file = "services.conf",
  .password_file = "passwords.conf",
  .system_files = false,
});
```

`load` is synchronous and explicit; parse/connect never invoke it automatically.
Defaults enable PostgreSQL environment variables and user/system discovery.
Disable all three source flags to exclude ambient sources; explicit paths still
work. An omitted user is resolved from the effective OS account even then.
Do not mutate the process environment concurrently with loading.

Precedence is connection string, selected service, environment, then defaults.
Connection-string duplicates use the last value; service duplicates use the
first. A service uses literal `key=value` lines, not connection-string quoting;
nested services fail. User services do not inherit missing system-service keys.

The service selector is string `service`, `ConfigSources.service`, then
`PGSERVICE`. Explicit service paths beat `PGSERVICEFILE`; automatic user
paths are `HOME/.pg_service.conf` or `APPDATA/postgresql/.pg_service.conf`.
If absent, system selection uses an explicit path, `PGSYSCONFDIR`, then
the compiled `WEAVE_POSTGRES_SYSCONFDIR`. Relocating an installation does not
change that compiled path. No service selector means no service-file access.
Malformed selected services and unknown service names fail.

A nonempty password avoids password-file access. Otherwise, string `passfile`
takes precedence over explicit loader path, service and `PGPASSFILE`.
Automatic paths are `HOME/.pgpass` and `APPDATA/postgresql/pgpass.conf`.
Missing automatic files are ignored; missing explicit files fail.
Entries use `host:port:database:user:password`: first match per host wins;
`*` is a wildcard, with escaped colon/backslash and literal escaped asterisk.
Match the configured name, or hostaddr when unnamed, not the resolved peer.
Physical replication uses database field `replication`.
Typed `Host.password` overrides the global password for that host.

Service/password files must be regular and at most 1 MiB. Linux password files
must exclude all group/world access, checked on the opened descriptor; insecure
files fail instead of being silently ignored. Windows paths are UTF8 and rely
on deployment ACLs, not a claimed password-file ACL validator. Protect service
files with credentials equally. Caller Options retain caller-owned secrets;
library cleansing is best-effort, not protection from every copy or crash dump.

`PGDATESTYLE`, `PGTZ`, `PGGEQO` and bounded typed `Options.settings` supply
startup settings; reserved identity/protocol fields cannot be overridden.

### libpq Migration Profile

`Options::load(text, {.libpq_compatibility = true})` explicitly selects
`sslmode=prefer`, protocol 3.0, legacy `requiressl` and conventional
`root.crt`, `root.crl`, `postgresql.crt`, `postgresql.key` discovery
under `HOME/.postgresql` or `APPDATA/postgresql`.
Explicit settings win; `user_files=false` disables automatic discovery.
Linux private keys require owner-private permissions or root ownership with
only group-read access; Windows requires deployment ACLs.

This is a migration subset, not unrestricted libpq compatibility.
It preserves bounded deadlines, explicit providers, TCP localhost and the
security restrictions below. It never prompts for a secret, installs a
hardware/ENGINE provider, fetches revocation data or infers a PostgreSQL install.

## TLS Modes

Supply either immutable `Options::tls` or loading policy `tls_options`, not
both. `sslrootcert=system` requires verify-full. Direct negotiation
(`sslnegotiation=direct`) requires TLS at least require, with PostgreSQL ALPN.
Credential/provider failures are terminal and never authorize host failover,
even if their error code resembles timeout or an unsuitable session.

| Mode | Policy |
| --- | --- |
| `verify-full` | Default: trusted chain and DNS/IP identity |
| `verify-ca` | Trusted chain without hostname checking |
| `require` | TLS; explicit root also requires chain verification |
| `prefer` | TLS first, strictly bounded plaintext fallback |
| `allow` | Plaintext first, upgrade only after pre-credential SQLSTATE 28000 |
| `disable` | Plaintext, without TLS settings or required channel binding |

Weak modes do not provide authenticated-server guarantees. A supplied context
must meet the mode's minimum policy and may be stronger. TLS metadata reports
what was actually verified; unverified certificates are not authenticated identities.

Prefer can accept a valid SSLRequest N or retry a failed TLS handshake on a fresh
socket pinned to the same numeric endpoint. Certificate/revocation/credential
failure, malformed negotiation, authentication rejection, cancellation and
deadlines **never authorize downgrade**. Allow does not retry invalid-password
rejection or anything after credentials/AuthenticationOK. Both attempts share
one deadline. Fallback is prohibited for required channel binding/client identity,
GSS encryption, OAuth and availability probes.
See [TLS](../../tls/README.md) for credentials, SNI, mTLS, CRLs and key-log policy.

SNI defaults on for DNS names, never numeric IPs. Disabling SNI does not disable
hostname verification. Unix sockets require explicit local transport and policy;
`requirepeer` is an explicit loader-supported local peer check, not TCP auth.

## Authentication

Authentication policy restricts allowed methods. `require_auth` can express
allow/deny lists; an accepted startup must satisfy the configured policy.
Cleartext passwords require explicit opt-in and verified TLS/protected GSS;
MD5 is deprecated and requires opt-in. SCRAM-SHA-256 and SCRAM-SHA-256-PLUS
verify the server proof. `ChannelBinding::require` rejects non-bound methods,
including trust authentication. Weak TLS modes do not bypass these rules.

### Passwords

`ScramKey::parse` accepts padded base64 for exactly 32 bytes. Client/server
keys are salt/iteration-specific middleware credentials, not general passwords.
Both keys bypass password derivation; a missing key requires a password.
A client key alone without a password is rejected because server proof cannot
be verified. Set an explicit SCRAM-only policy if other methods must be forbidden.
Key moves/destruction cleanse owned bytes; caller input/copies remain caller-owned.

All verifier/change-password APIs take user first, password second. The free
`password_verifier` is synchronous, defaults to SCRAM-SHA-256 with fresh salt
and 4,096 iterations, and needs no Connection. Member verifier/change_password
can consult SHOW password_encryption or take explicit PasswordOptions.
MD5 selection requires explicit legacy opt-in, separately from authentication policy.
Iterations are bounded to 1-1,000,000; inputs to 64 KiB without NUL. SCRAM uses
SASLprep with PostgreSQL's raw-byte fallback for prohibited/unassigned input.

The async member factories own inputs before suspension. One session lease covers
lookup/mutation; typed SQL contains the verifier, not plaintext, and its trace is
redacted. Returned verifier strings are caller-owned and not automatically cleansed.
Hashing is synchronous CPU work and can delay cancellation. An interrupted ALTER
may have committed: no retry, automatic rollback or reconnect is implied.
Use protected transport; hashing does not make plaintext safe. TLS passphrase
providers have their own [credential policy](../../tls/README.md#credentials).

### GSS

GSS is optional at build time and requires an explicit `GssContext` provider.
Its bounded native credential workers are separate from networking execution;
ordinary connections do not create these threads. Capture credentials during
setup: Linux supports system-default or explicit FILE caches; Windows captures
the effective token without silently falling back after impersonation failure.
Anonymous/identification tokens and unsupported cache types are rejected.
Credential cache contents can still change through external renewal.

Use the real hostname and service name even when pinning a numeric address.
Mutual authentication defaults on; disabling it is allowed only with verified
TLS. Kerberos authentication is not traffic encryption. Windows requires
Kerberos rather than accepting NTLM substitution; delegation is off by default.

`gssencmode=disable` is default. Prefer/require opt into protected GSS records,
ahead of TLS. Prefer can fall back on server refusal, not after failed
cryptographic negotiation or active-record errors. Encryption requires mutual
Kerberos/confidentiality. Native provider capacity covers live encrypted
connections and in-flight cleanup; exhausted admission fails, never creates
unbounded workers. Domain/KDC/SPN/service-account deployment still needs
environment-specific validation.

### OAuth

OAuth requires an explicit owning `OAuthProvider`; parsing policy does not
install one. Custom token acquisition is an async, noexcept callback returning
an owning `OAuthToken`. The application owns issuer trust, token validation,
expiry and storage. Connection options include issuer, client ID, secret and
scope; secrets are retained only by their owning configuration/provider state.

Issuer is bounded HTTPS without userinfo/query/fragment and matches literal
configured spelling. Explicit discovery URLs must be a permitted well-known
form for that issuer; server-selected alternatives are rejected.
Absent scope may use discovery; an explicitly empty scope overrides it.

Optional synchronous cached-token lookup must be memory-only/nonblocking.
It is used only with an explicit discovery URL, protected transport and permitted
OAUTHBEARER. A miss performs discovery/acquisition then a pinned reconnect;
a rejected cached token does not silently refresh or fail over.

The device provider awaits an owning prompt handler; display verification URI
**and user code**. Complete URI is a browser convenience, not a required HTTP
request. No browser is opened or secret printed automatically.
HTTP uses its own verified TLS snapshot and HTTP/1.1, not PostgreSQL TLS state.
Trusted metadata may name other HTTPS hosts, but redirects/decompression,
JWT verification, refresh-token/DPoP flows and a general HTTP client are not supplied.

Client authentication automatically prefers advertised Basic then POST;
explicit methods must be advertised. None with a secret fails. Basic form-encodes
both values before base64; only one method is sent, without failure fallback.
Connection secrets override factory defaults only when nonempty.
Polling honors interval, authorization_pending and slow_down, backs off on
transport timeout, and stops at the bounded deadline. Cancellation drains
prompt/provider/network work. Secret and JSON/HTTP input limits fail rather
than truncate; provider deployment remains an application trust boundary.

### LDAP Services

LDAP is a synchronous configuration lookup, enabled both by
`WEAVE_POSTGRES_LDAP=ON` and `ConfigSources.ldap=true`.
It supports anonymous plaintext ldap only, not LDAPS, StartTLS, bind credentials,
referrals or aliases. Use a trusted directory/network; configuration can contain
credentials and is not authenticated transport.

Require one matching entry and at least one value. Directory values use
keyword-string grammar, joined by newlines; ordinary service-file values stay
literal. Explicit connection settings win over service/directory, then environment.
Only connect/bind unavailability permits a later directory fallback; search or
malformed-result failures are terminal. Nested service references are rejected.
The positive lookup timeout is at most one minute, with at most 16 attempts;
native DNS can block beyond it and Task cancellation cannot interrupt loading.

## Host Selection And Liveness

A nonempty `hosts` list replaces host/port. Numeric `Host.address` pins TCP
without changing verification/SNI name. Ordered traversal is default; random
shuffles hosts once, then each host's addresses. Exhaust one host before the next.
Prefer-standby makes a second pass accepting any suitable server.
Targets include any, read_write, read_only, primary, standby and prefer_standby;
checks leave the session idle but do not guarantee future role/configuration.

A positive connect timeout covers DNS, addresses, startup and session checks
**per configured host**, not per address or the entire host list.
Wrap connect in `timeout` for a whole-selection budget.
Unreachable hosts, attempt deadlines and unsuitable sessions permit failover;
authentication, verification and protocol failures stop it.
Queries are never balanced/replayed across servers.

TCP keepalive defaults on; zero tuning retains OS defaults.
`tcp_user_timeout` is Linux-only for nonzero values and is not a query deadline.
Socket configuration failures are not disguised as an unreachable host.
`ping` / `ping_blocking` report accepting, rejecting or no_response without
answering authentication or issuing SQL. Accepting is startup evidence, not
proof of usable credentials; invalid policy/certificate/protocol failures stay errors.

## Cancellation And Reset

`cancel_handle()` snapshots the selected peer/backend key independently.
`CancelHandle::request()` sends PostgreSQL CancelRequest; await the original operation
to observe SQLSTATE and drain it. Cancellation does not prove rollback or that
side effects did not commit. It is not ordinary Task cancellation.

`reset(Options)` replaces a drained, unpooled session using fresh explicit
configuration; no automatic reconnect/replay occurs. Deferred Tasks count as
borrows even before they execute. A captured CancelHandle cannot be retargeted
by later reset. Local cancellation during active protocol work retires the
connection after draining native operations.

For owning attempt history, optional `ConnectionReport` on connect/reset
records actual destinations, bounded diagnostics and phase timing.
[Diagnostics](observability.md#diagnostics).
