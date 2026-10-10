# PostgreSQL Connections

For owning host/address failure history, use the optional
[ConnectionReport](postgres-diagnostics.md#connection-reports) output on connect
or reset. Existing error-code propagation and Diagnostic overloads are unchanged.

`pg::connect(Options)` is a lazy Task. It owns its configuration, performs
nonblocking DNS/TCP/TLS or local-socket startup on the executing Context, and returns an owning
Connection. The blocking facade runs the same engine on its own caller-thread
Context. No hidden runtime or I/O thread is created.

Network startup is asynchronous, but construction from `Options::tls_options`
loads credential files synchronously on the executing thread. Its
[`private_key_password_provider`](tls.md#passphrase-providers) is retained by the
owning Options before the Task's first suspension, including unstarted/rejected
Tasks, and discarded with credential construction. Use a prebuilt `Options::tls`
context to keep credential lookup/loading out of worker execution. Provider
failures are terminal for connect, reset and ping, even when the callback returns
`timed_out`, a connection error or `target_session`: they are not evidence that
another host should be tried. Configuration inspection exports only a provider-
presence flag, never its callable or credentials. Parsing/loading connection
strings does not register callbacks or invoke a secret provider.

## Connection Strings

```cpp
auto options = weave::pg::Options::parse(
  "postgresql://app@db.example/app?sslmode=verify-full&sslrootcert=ca.pem");
if (!options)
  co_await weave::fail(options.error());

options->password = password;
auto connection = co_await weave::pg::connect(std::move(*options));
```

`Options::parse` is synchronous and returns owning values. It accepts PostgreSQL
keyword/value syntax, including quoted values and backslash escapes, or
`postgresql://` / `postgres://` URIs. URI components are percent-decoded; a `+`
remains a plus, not a space. Bracket IPv6 authorities and encode reserved
characters. Duplicate settings use the last value; URI query settings override
authority/path settings.

Parsing never opens credential files, reads environment variables, looks up an
OS username or performs I/O. Empty input returns Weave defaults, including an
empty user; supply the user before connecting. An omitted database uses the
user at startup. Empty/missing hosts use **TCP localhost on both platforms**,
not libpq's Linux Unix-socket default.

### Option Schema

`<weave/postgres/configuration.hpp>` exposes the synchronous, allocation-free
`pg::option_schema()`. Its immutable span contains keyword names, environment
names, optional built-in defaults, secret-field markers and value/platform
constraints. The views refer to static library storage, not a Connection or
caller-owned configuration. It does not read environment variables or files.

```cpp
auto schema = weave::pg::option_schema();
auto mode = std::ranges::find(
  schema, "sslmode", &weave::pg::OptionDescriptor::keyword);
```

| Support | Meaning |
| --- | --- |
| `parse` | Recognized by both `Options::parse` and `Options::load`, subject to constraints |
| `load` | Recognized only by the explicit loader, such as `service` or `passfile` |
| `unsupported` | Recognized but rejected; a documented loader exception can still apply |
| `unrecognized` | Included for native-reference completeness, but not accepted as a Weave keyword |

An empty default is distinct from no independent keyword default. Loader,
provider and OS resolution can still supply values; for example, load resolves
an empty user from the OS. `PGSERVICEFILE` is a loader selector even though the
`servicefile` keyword is unsupported. The conditional `PGREQUIRESSL` legacy
path remains separate. Limits, authentication restrictions and TLS verification
are not weakened by descriptor availability. A supported keyword does not imply
every native libpq value or provider is supported.

The schema contains all 50 keywords observed in the pinned libpq 18.4/18.6 builds
plus the three existing Weave aliases `servicefile`, `ssl` and `requiressl`.
SCRAM keys are marked secret even though native libpq labels them advanced/debug
fields. `sslkeylogfile` is an explicit sensitive opt-in, with no ambient environment
mapping; [key logging](tls.md#sensitive-key-logging) defines its security and I/O contract.
Defaults describe Weave, not libpq: notably verified TLS, TCP localhost, a
30-second connect timeout, UTF8 and an application name of `weave`.

This is not a resolved Options dump, a live `PQconninfo` counterpart or a
round-trippable representation of an opaque supplied TLS/GSS context. User-supplied
application/options text is not sanitized merely because its field is nonsecret;
never log arbitrary configuration automatically. Dynamic resolved-option/session
snapshots remain separate work. [Exact qualification](postgres-qualification.md#static-option-schema-2026-10-09).
For native descriptor semantics see the
[PostgreSQL connection-control reference](https://www.postgresql.org/docs/18/libpq-connect.html).

Supported keys:

- `host`, `hostaddr`, `port`, `user`, `dbname`, `password`.
- `application_name`, `fallback_application_name`, `options`, `client_encoding`.
- `connect_timeout`, `channel_binding`, `target_session_attrs`, `replication`.
- `require_auth`, `min_protocol_version`, `max_protocol_version`.
- `scram_client_key`, `scram_server_key`.
- `keepalives`, `keepalives_idle`, `keepalives_interval`, `keepalives_count`,
  `tcp_user_timeout` (nonzero values require Linux).
- `sslmode`, `sslrootcert`, `sslcert`, `sslkey`, `sslpassword`, `sslcrl`, `sslcrldir`,
  `ssl_min_protocol_version`, `ssl_max_protocol_version`.
- `sslnegotiation=postgres` (default) or `direct`; see the qualification scope below.
- `load_balance_hosts=disable` (default) or `random`; `gssencmode=disable`
  (default), `prefer` or `require`. Encryption needs an explicitly attached
  [GSS provider](postgres-gss.md#encrypted-connections).

`sslcrldir` and explicitly loaded `PGSSLCRLDIR` select the trusted OpenSSL
hashed CRL directory; missing or invalid required CRLs fail the handshake.
The typed `Options::tls.private_key_format` also supports PEM, DER and configured
OpenSSL STORE locators. STORE uses `private_key_file` as the locator and redacts
that entire value in `Options::info()` and session snapshots because URIs can
contain credentials. Format selection is explicit, not inferred from `sslkey`.
There is no legacy ENGINE compatibility or implicit hardware-provider setup.
[TLS credential and revocation contracts](tls.md#server-and-adapters).

`replication=true` (also `on`, `yes`, `1`) selects physical startup;
`database` selects database-scoped replication, while `false`, `off`, `no`, `0`
disable it. Values are case-insensitive and map to `Options::replication`.
[Replication transport](postgres-replication.md) defines stream ownership and limits.

Host and address lists must align; a single port applies to every host. Empty
list elements use the default name/port, or the supplied numeric address as
the verification name. `connect_timeout` is positive integral seconds, at most
86400; Weave does not accept an infinite timeout. Application names fall back
to `fallback_application_name` and then `weave` when empty.

TLS supports `verify-full` (the default), `verify-ca`, `require`, `prefer`, `allow`
and `disable`, through `Options::tls_mode` or `sslmode`. TLS file paths
are stored in `tls_options`; loading occurs during connection establishment.
Credential loading uses synchronous filesystem access. For latency-sensitive
workers, create `TlsContext` during application setup and supply `Options::tls`
instead of loading files on a worker's connection path.
Cooperative deadlines cannot interrupt a synchronous credential-file read.
`sslrootcert=system` uses OpenSSL's default trust paths. CRLs check the chain;
protocol bounds support TLSv1.2/1.3. A plaintext configuration with explicit TLS
credentials or required channel binding is rejected. Do not set both `tls`
(an already created credential snapshot) and `tls_options`.

| Mode | Transport and server verification |
| --- | --- |
| `verify-full` | TLS, trusted chain and DNS/IP identity |
| `verify-ca` | TLS and trusted chain, no hostname check |
| `require` | TLS; an explicit root file/directory also enables chain verification |
| `prefer` | TLS first; plaintext only under the bounded fallback policy below |
| `allow` | Plaintext first; upgrade only when the server rejects it with pre-credential SQLSTATE 28000 |
| `disable` | Plaintext; no TLS configuration or required channel binding |

Weak modes are compatibility choices, not authenticated-server guarantees.
`sslrootcert=system` requires `verify-full`; direct TLS requires at least `require`.
Supplied TLS contexts must meet the mode's minimum verification policy and may
be stronger. TLS metadata separately reports certificate/name verification;
unverified certificates are not exposed as authenticated peer identities.

`prefer` accepts a valid SSLRequest `N` on the original socket, or retries a failed
TLS handshake on a fresh socket pinned to the same numeric endpoint. Verification,
revocation, credential setup, malformed negotiation, authentication rejection,
cancellation and deadlines do not authorize downgrade. `allow` does not retry
invalid-password rejection or a failure after credentials/AuthenticationOK.
Fallback is unavailable for required channel binding/client identity, configured
GSS encryption, OAuth or availability probes. Both attempts share the original
connect deadline. Reports retain the first failure and the final attempt.
This is intentionally stricter than unrestricted native libpq fallback.
Cleartext passwords, OAuth and non-mutual GSS still require verified-hostname TLS
or their existing protected-GSS policy; a weak mode does not bypass those rules.
[Native TLS mode reference](https://www.postgresql.org/docs/18/libpq-ssl.html).

Explicit `client_encoding` is sent at startup and checked by PostgreSQL.
Use the typed `set_client_encoding(Encoding)` Task or the blocking facade to
change it later; the synchronous getter follows reported session metadata.
[Encoding utilities and quoting](postgres-encoding.md).
`auto` is unsupported; escaping helpers still require UTF8. Parameters/results
in other encodings are bytes, not automatically transcoded strings.

Malformed or unknown settings return `invalid_argument`. Pure parsing rejects
`service`, `passfile` and username-based `requirepeer` with
`operation_not_supported`; use explicit `Options::load` for these setup operations.
Direct TLS negotiation has permanent ALPN, configuration, reconnect-refusal and
synthetic startup/reset/cancellation/pinned-OAuth regressions on nine profiles.
Real mTLS/SCRAM-PLUS startup/reset/query-cancellation and independent libpq
controls also pass those profiles; permanent GSS-priority passes four Linux
profiles. Broader release/deployment qualification remains. GSS requires an explicit provider;
parsing its policy does not create one.
The input is bounded to 1 MiB, each decoded field to 64 KiB and hosts to 64.
Do not log connection strings containing passwords or SCRAM keys. Internal cleanup is
best-effort, not a guarantee that every allocator/SSO copy is erased.

Connect and reset Tasks own a private cleansing wrapper before their first
suspension. Dropping an unstarted Task, rejected Context/Runtime submission,
pre-start cancellation and setup failure therefore cleanse its owned global,
per-host and TLS private-key passwords before releasing their storage. Internal
moves also clear retained inline source characters on the qualified MSVC/GCC
string implementations. This adds no shared allocation or wrapper coroutine.
The blocking facade protects its options before Context creation as well.

Public `Options` remains a normal copyable value. Passing it by value can leave
caller-owned copies, including bytes retained by a moved-from string; callers
remain responsible for those. These checks do not promise secure allocation,
complete erasure of protocol/cryptographic temporaries or protection from crash
dumps and swapping.

Private password/MD5/SCRAM buffers additionally use a cleansing allocator that
clears complete owned allocations during growth, replacement and destruction.
SASLprep's owned ICU work buffers follow that policy; fixed digest/key/proof
intermediates are move-only and clear moved-from values and destruction storage.
The shared serializer keeps ordinary query buffers unchanged and selects
cleansing storage only for authentication responses. OpenSSL and ICU still own
their internal temporary storage; no locked memory or secure-heap guarantee is
made. See the [qualification record](postgres-qualification.md).

## Direct TLS

```cpp
options.tls_negotiation = weave::pg::TlsNegotiation::direct;
auto connection = co_await weave::pg::connect(std::move(options));
```

The same setting is accepted through `sslnegotiation=direct` in keyword/URI/service
configuration and `PGSSLNEGOTIATION` in explicit environment loading. The default
`postgres` mode sends SSLRequest before upgrading; `direct` starts with TLS and
offers only the mandatory `postgresql` ALPN label, even with a prebuilt credential
snapshot offering other protocols. A missing/different selection fails before
Startup or cancellation-key payloads. Plaintext plus direct negotiation is invalid;
Weave still requires certificate-chain and hostname verification.

GSS encryption retains priority. An explicit GSS decline under `prefer` requires
a fresh TCP connection to the same selected endpoint before direct TLS; native
socket controls are reapplied. Required GSS never falls back. Direct handshake
failures, including timeouts, do not downgrade or try another host. Initial TCP
availability failures retain the existing host-selection policy.

Reset and independent cancellation snapshots retain the negotiation mode. OAuth
discovery/reconnect retains it while pinning the selected endpoint. Owning
OptionsInfo reports requested policy; ConnectionInfo reports actual security/ALPN,
which can be GSS instead of TLS. These are not native connection-handle exports.

## Server Name Indication

`Options::server_name_indication` defaults to `true`. Set it to `false`, or use
`sslsni=0` in keyword/URI/service configuration, to omit the client TLS routing
extension. `sslsni=1` enables it; other nonempty values are invalid. Explicit
environment loading recognizes `PGSSLSNI`. Empty values retain the default,
including an empty Windows environment variable.

This never disables certificate-chain or hostname/IP verification. Numeric IP
verification names already omit SNI. The setting applies to both direct and
SSLRequest-based TLS and does not mutate a supplied credential snapshot. Reset
uses its new Options; independent cancellation handles retain the original
session's policy even after reset. Endpoint-pinned OAuth reconnects retain their
own startup policy. Plaintext, local and GSS-encrypted transports do not use SNI.

`OptionsInfo` reports the requested setting, not evidence of a transmitted
extension. Permanent Windows controls independently inspect actual ClientHello
SNI, startup/reset/cancellation and custom-provider OAuth, including rejection
without host fallback. Linux Debug/Release development controls also pass;
Windows public resumed-stream adapters now also have permanent native-peer and
ASan soak coverage. Remaining Linux and real-server SNI gates are still open.
[Qualification scope](postgres-qualification.md#sni-stream-adapter-regressions-2026-10-10).

[Development evidence](postgres-qualification.md#direct-tls-development-2026-10-10)
and [permanent regression scope/remaining gates](postgres-qualification.md#direct-tls-server-regression-qualification-2026-10-10).
The [PostgreSQL protocol reference](https://www.postgresql.org/docs/18/protocol-flow.html)
requires ALPN for direct TLS; this is a server capability introduced in PostgreSQL
17, not a compatibility fallback for older servers.

## Client Certificate Policy

Set `Options::client_certificate`, or parse/load
`sslcertmode=disable|allow|require`. Explicit environment loading recognizes
`PGSSLCERTMODE`; empty values retain the default `allow`. The same mode is
reported by the nonsecret configuration snapshot.

`disable` suppresses client identity transmission. For file-loaded credentials,
it also ignores certificate/key paths and does not invoke a key-password provider;
trust, revocation and version policy are still enforced. Requested paths remain
in the configuration snapshot. Prebuilt TLS credentials stay immutable and have
identity use disabled only on the individual handshake.

`require` needs a requested, usable TLS identity, not merely a configured file.
Weave fails with `TlsError::client_certificate_required` before PostgreSQL Startup
when that requirement is absent. Plaintext, local and GSS-encrypted transports
cannot satisfy it. libpq checks its requirement when authentication succeeds;
Weave deliberately rejects earlier. Neither behavior proves server authorization
or correct validation of the client certificate.

The policy applies to direct and SSLRequest negotiation, pinned OAuth reconnects
and reset. Independent cancellation handles retain their original mode, even
after reset selects a different one. Security-policy failure is terminal, not a
reason to try a weaker transport or another host.

[Permanent Windows regression and native-control evidence](postgres-qualification.md#client-certificate-policy-regressions-2026-10-10).
Permanent Windows real-server/native controls and final affected Debug/Release/ASan
reruns now pass; [latest platform qualification](postgres-closure.md). The reference contract is PostgreSQL's
[sslcertmode documentation](https://www.postgresql.org/docs/18/libpq-connect.html#LIBPQ-CONNECT-SSLCERTMODE).

## Local Sockets

```cpp
auto connection = co_await weave::pg::connect({
  .host = "/run/postgresql",
  .user = "app",
  .database = "app",
  .plaintext = true,
  .required_peer_user = server_uid,
});
```

An absolute directory in `host` selects a local socket at
`<directory>/.s.PGSQL.<port>`. Windows also accepts rooted drive paths such as
`C:/postgres`; Linux accepts an abstract directory prefix such as `@postgres`.
The complete generated address must fit 107 bytes, matching PostgreSQL's socket
path bound. URI hosts can be percent-encoded, or supplied through `?host=...`.
Paths are owned and never created, removed or unlinked by the client. Use a
directory controlled by the deployment, not an untrusted shared directory.

Local transport requires **explicit `plaintext = true` / `sslmode=disable`**.
Unlike libpq, Weave does not silently ignore TLS requirements on local sockets.
Explicit TLS credentials, required channel binding, numeric `hostaddr` overrides
and nonzero TCP-only tuning are rejected instead of ignored. An omitted host
still selects TCP localhost; there is no PostgreSQL installation-path discovery.

`required_peer_user` is an optional **Linux kernel UID**, not a PostgreSQL role or
OS username. It is checked before any startup data or credentials are sent. A
mismatch is terminal: it never falls through to another host. A UID requirement
with any TCP destination is invalid, and Windows reports
`operation_not_supported` before network setup.
Filesystem sockets are supported on both backends; Windows abstract sockets and
peer-credential queries remain explicitly unsupported by the qualified Local backend.

Linux `Options::load` supports `requirepeer=postgres` in keyword/URI options,
service files and `PGREQUIREPEER`. Explicit options override the service, which
overrides the environment; an explicit empty value disables the requirement.
For example, call this during synchronous application setup, before driving workers:

```cpp
auto options = weave::pg::Options::load(
  "host=/run/postgresql user=app dbname=app sslmode=disable requirepeer=postgres");
if (!options)
  return weave::report_error(options.error());
```

Loading resolves the name through reentrant NSS functions and checks its canonical
reverse mapping before snapshotting the UID into `required_peer_user`. Missing
accounts, aliases to another canonical name, lookup errors and exhausted bounded
lookup storage fail explicitly. NSS can contact directory services and block;
this is synchronous setup, not a Task deadline or an I/O-worker operation.
Pure `Options::parse` never performs the lookup. Nonempty Windows name policy
returns `operation_not_supported`; empty policy performs no lookup on either OS.

This differs deliberately from
[libpq's connection-time peer-name lookup](https://www.postgresql.org/docs/18/libpq-connect.html#LIBPQ-CONNECT-REQUIREPEER):
Weave checks the frozen UID on connects/resets and never consults NSS on the I/O
path. Reload Options after account/policy changes; UID reuse is not distinguished.
TCP destinations remain invalid with a peer requirement instead of ignoring it.
Neither the snapshot nor canonical lookup authenticates a specific process or
prevents a deployment administrator from changing account mappings.

Local CancelHandles retain the actual connected socket address, not a DNS name
or a TCP fallback. On Linux they also retain and recheck the connected peer's UID
before sending the cancellation secret, even when the original Options had no
explicit UID requirement. This is an account check, not process identity: another
server with the same UID is not distinguished. Protect the containing directory.
The same behavior is available through `BlockingConnection` and
`CancelHandle::request_blocking()` outside an executing Context.

Multi-host traversal, target-session checks, reset, queries, pipelines, COPY and
row streaming use the same protocol engine. Explicit password-file loading matches
the supplied directory name, including escaped drive colons on Windows. Weave has
no compiled-in PostgreSQL default socket directory to map to the special libpq
`localhost` password-file alias. Link only `weave::postgres`; its private Local
dependency is resolved by the installed CMake package.

## Authentication Policy

```cpp
options.authentication = {
  .methods = {weave::pg::Authentication::scram_sha256},
};
options.min_protocol = weave::pg::ProtocolVersion::v32;

auto connection = co_await weave::pg::connect(std::move(options));
auto method = connection.authentication_method();
auto version = connection.protocol_version();
```

`AuthenticationPolicy` restricts the server's chosen method; it does not select
one. An empty default policy adds no restriction. A nonempty list allows only
those methods; `exclude = true` rejects them instead. An empty exclusion list is
invalid. The server must complete one exchange: repeated or switched initial
challenges and incomplete SCRAM exchanges fail. `authentication_method()` reports
the completed exchange, not the HBA label: an HBA `md5` rule can negotiate SCRAM
when the role stores a SCRAM secret. `none` means no wire challenge, not the
absence of TLS client-certificate verification.

`require_auth` accepts comma-separated `none`, `password`, `md5`,
`scram-sha-256`, `gss`, `sspi`, and `oauth`. Prefix **every** entry with `!` for
exclusion; mixed positive/negative lists, empty entries and unknown labels are
invalid. Positive GSS/SSPI requirements need an explicit `Options::gss` provider
before network setup. Inclusive OAuth requirements need configured
`Options::oauth` and an explicit owning provider before I/O; see
[OAuth configuration and scope](postgres-oauth.md). Native methods can be excluded
without enabling them.
An encrypted GSS transport satisfies implicit GSS authentication when no separate
challenge is sent. See the
[PostgreSQL policy model](https://www.postgresql.org/docs/18/libpq-connect.html#LIBPQ-CONNECT-REQUIRE-AUTH).

Cleartext password and MD5 remain disabled by default. Unlike libpq,
`require_auth=password` or `require_auth=md5` **does not enable them**: explicitly
set `allow_cleartext_password` or `allow_md5_password`. Cleartext password is sent
only over verified TLS or mutually authenticated GSS encryption, even with its
opt-in. MD5 is deprecated; prefer verified
TLS plus SCRAM-PLUS. Rejected challenges produce no credential response;
authentication/protocol failures never fail over to another configured host.
TLS verification and `channel_binding=require` remain independent constraints.

## Protocol Bounds

`min_protocol` and `max_protocol` accept `ProtocolVersion::v30` or `v32`.
Connection strings accept `3.0`, `3.2`, and `latest` (currently `3.2`) through
`min_protocol_version` / `max_protocol_version`. The minimum cannot exceed the
maximum. Weave retains minimum 3.0 and requested maximum 3.2 defaults, unlike
libpq's ordinary 3.0 request default. `protocol_version()` exposes the negotiated
version on both asynchronous and blocking connections.

A downgrade below the minimum returns `protocol_not_supported` before
credentials, without host failover. Unsupported, repeated, truncated, or
out-of-order negotiation is a protocol error. Protocol 3.0 requires four-byte
cancellation keys; 3.2 supports the bounded variable-length key.
`PGREQUIREAUTH`, `PGMINPROTOCOLVERSION`, and `PGMAXPROTOCOLVERSION` are read only by
explicit `Options::load`, with string/service/environment precedence.

## Transport Liveness

`Options::keep_alive` enables TCP keepalive by default, like libpq. Set
`enabled = false` to disable it. `idle` and `interval` use integral seconds;
`probes` is a count. Zero values retain the fresh socket's OS defaults.
Disabled keepalive does not apply tuning, but invalid numeric fields are still
rejected. Connection-string `keepalives=0` disables it; other nonnegative integers
enable it. All tuning values must fit a nonnegative signed 32-bit integer;
native platforms enforce their additional limits during connection setup.

`Options::tcp_user_timeout` uses integral milliseconds. Zero means leave the
fresh socket's OS default unchanged. Nonzero values require Linux's native
TCP_USER_TIMEOUT; Windows parsing and typed connection setup reject them with
`operation_not_supported` before DNS or TCP. This is not a query deadline or
an idle-session timer: it limits unacknowledged/unsent TCP data according to
the [native TCP policy](https://man7.org/linux/man-pages/man7/tcp.7.html).

Settings are applied before TLS/startup and cannot silently fail over to another
host when socket configuration fails after connection. Modern Windows keepalive
tuning requires the [documented native option support](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-tcp-socket-options);
older systems return their native errors. See [TCP controls](tcp.md#socket-controls)
for synchronous setters/readback and partial-update semantics. These options do
not guarantee a peer is responsive, cancel a stalled SQL operation, or substitute
for application deadlines and PostgreSQL CancelRequest.

## Configuration Loading

```cpp
auto options = weave::pg::Options::load("service=app", {
  .service_file = "services.conf",
  .password_file = "passwords.conf",
});
```

`Options::load` is the explicit synchronous setup operation. Its default
`ConfigSources` enables PostgreSQL environment variables, per-user files and
compiled-in system service-file discovery;
`parse` and `connect` never perform this loading automatically. Load before
starting latency-sensitive workers: filesystem and OS account lookup are
synchronous, and cooperative cancellation cannot interrupt them.

To exclude ambient PostgreSQL variables and automatic user/system files, use
`{.environment = false, .user_files = false, .system_files = false}`. Explicit service/password paths
still work. An omitted/empty user is resolved to the effective OS account,
even with those sources disabled; use `parse` to avoid account lookup entirely.
Do not concurrently mutate the process environment while loading.

Connection-string settings override the selected service section, which
overrides environment defaults. Service-file duplicates use the first value;
connection-string duplicates still use the last. The first matching service
section wins. Values in service files are literal: do not apply connection-string
quoting/backslash syntax to them. Comments start with `#`; use `key=value`
without spaces around the equals sign. Nested services fail explicitly. LDAP
lookup requires both a library build option and a per-load opt-in; by default
it returns `operation_not_supported` rather than performing network I/O.
See [LDAP service lookup](postgres-ldap.md) for its separate directory-value
grammar, precedence, fallback, plaintext security policy and native bounds.

`service` in the string overrides `ConfigSources.service` and `PGSERVICE`.
`ConfigSources.service_file` selects a file ahead of `PGSERVICEFILE`; otherwise
per-user discovery uses `HOME/.pg_service.conf` on Linux or
`APPDATA/postgresql/.pg_service.conf` on Windows. If the service is absent there,
`system_service_file`, then `PGSYSCONFDIR/pg_service.conf`, then the compiled-in
directory supplies the system file. A user service does not inherit missing
keys from the same-named system service. No service name means no service-file
access; malformed selected services fail instead of falling back.

Build-time `WEAVE_POSTGRES_SYSCONFDIR` selects that absolute UTF8 directory.
It defaults to CMake's full install sysconf directory; an empty value disables
the compiled-in default. To share a PostgreSQL installation's service file,
configure it to the directory reported by `pg_config --sysconfdir`. Weave does
not require PostgreSQL tools or invoke them during build/runtime. The path is
fixed in the library, not inferred from an application executable or changed
by relocating an installed package. `PGSYSCONFDIR` or an explicit file supplies
a deployment override. The generated path header is private and not installed.

`system_files = false` disables only compiled-in discovery, just as
`user_files = false` disables home-file discovery. Explicit paths still work,
and `environment = true` still permits `PGSERVICEFILE`/`PGSYSCONFDIR` overrides.
Missing compiled-in files are ignored; an unknown selected service still fails.

Password selection happens after merging settings. A nonempty password avoids
file access. Otherwise, the connection-string `passfile` selects a file ahead
of `ConfigSources.password_file`, service settings and `PGPASSFILE`. Default
discovery uses `HOME/.pgpass` or `APPDATA/postgresql/pgpass.conf`. Missing automatic
files are ignored; missing explicitly selected files and unknown service names
are errors. Linux falls back to the effective account's home if HOME is absent.
Windows home-file discovery requires APPDATA.

Password entries use `host:port:database:user:password`. The first matching entry
wins for each host; `*` matches any value in the first four fields. Escape colons
and backslashes, including IPv6 colons. An escaped `\*` is literal, not a wildcard.
The host name (or hostaddr when the name is omitted) is matched, not the resolved
peer. Numeric ports are normalized to their typed value; an omitted database
matches the user. Physical replication instead matches the literal database
field `replication`; database-scoped replication retains the actual database.
[PostgreSQL's password-file rules](https://www.postgresql.org/docs/18/libpq-pgpass.html)
define that matching behavior.
Malformed nonmatching entries are skipped; a malformed matching
password is rejected. Loaded per-host secrets live in `Host.password`; explicit
typed host passwords override the global password during connection selection.
The library clears attempt/retained session copies after use and does not retain
other hosts' passwords in the selected session. Caller-owned Options still own
their secrets, including when retained for a later reset.

Both file types require regular files and are capped at 1 MiB. Linux password
permissions are checked on the opened descriptor and must forbid all group/world
access. Unlike libpq's warning-and-ignore policy, an insecure file returns
`permission_denied`. Windows relies on deployment directory/file ACLs; it does
not claim to validate ACL policy. Paths are UTF8 on Windows and use native wide
file APIs. No warnings or credentials are printed by the loader.
Protect service files containing credentials with equivalent deployment policy.

Supported connection environment variables map to the same documented parser
keys (`PGHOST`, `PGHOSTADDR`, `PGPORT`, `PGDATABASE`, `PGUSER`, `PGPASSWORD`,
`PGOPTIONS`, `PGAPPNAME`, `PGCONNECT_TIMEOUT`, `PGCLIENTENCODING`,
`PGTARGETSESSIONATTRS`, `PGLOADBALANCEHOSTS`, `PGCHANNELBINDING`, `PGSSLSNI` and TLS controls).
Recognized unsupported auth/transport/protocol controls fail if not overridden.
`PGREQUIRESSL` remains unsupported outside the explicit migration profile and is
suppressed by an explicit SSL mode.
Locale-message paths do not affect this native client. Avoid PGPASSWORD for
sensitive deployments; prefer a protected password file or explicit secret provider.

`PGDATESTYLE`, `PGTZ` and `PGGEQO` become startup settings. Applications can also
supply `Options.settings` as an ordered name/value collection. It is bounded to
64 entries with 64 KiB fields and the complete startup-message limit. Reserved
identity/protocol fields cannot be overridden this way; PostgreSQL validates
the GUC names/values.

Reference semantics: [environment](https://www.postgresql.org/docs/18/libpq-envars.html),
[services](https://www.postgresql.org/docs/18/libpq-pgservice.html) and
[password files](https://www.postgresql.org/docs/18/libpq-pgpass.html).

### Explicit libpq Migration Profile

```cpp
auto options = weave::pg::Options::load(
  "service=application",
  {.libpq_compatibility = true});
if (!options)
  return weave::report_error(options.error());
```

The opt-in profile reuses the loader's service/environment/password precedence.
It defaults to `sslmode=prefer` and protocol 3.0, recognizes legacy `requiressl=0|1`,
and discovers conventional `root.crt`, `root.crl`, `postgresql.crt` and
`postgresql.key` under `HOME/.postgresql` on Linux or `APPDATA/postgresql` on
Windows. Explicit settings win. `sslrootcert=system` still selects `verify-full`.
`user_files=false` disables automatic credential discovery, not explicit paths.
Missing required root/key files fail instead of silently switching trust sources.
Linux client keys must be private to their effective owner or root-owned with
only group-read access; Windows relies on deployment ACLs for client keys.

This is a migration subset, not a promise to accept every libpq configuration
unchanged. TCP localhost, bounded deadlines, explicit provider setup, UTF8
encoding, strict local-socket TLS policy and the security restrictions above
remain. It does not install GSS, legacy ENGINE or hardware providers, prompt for
passwords, infer PostgreSQL installation paths or fetch network revocation data.
Pure `Options::parse` and ordinary `connect` never activate this profile.
[Windows/Linux qualification and deployment boundaries](postgres-migration-qualification.md).

## Hosts and Session Selection

```cpp
weave::pg::Options options{
  .user = "app",
  .database = "app",
  .password = password,
  .hosts = {{"db-a.example", 5432}, {"db-b.example", 5432}},
  .target_session = weave::pg::TargetSession::read_write,
  .server_options = "-c search_path=pg_catalog",
  .host_balance = weave::pg::HostBalance::random};

auto connection = co_await weave::pg::connect(options);
```

An empty `hosts` list uses the existing `host` / `port` fields. A nonempty list
replaces that destination, with a maximum of 64 entries.
Each Host requires a nonempty name and nonzero port. Its optional `IpAddress`
pins the transport address and bypasses DNS; the name still determines TLS
verification/SNI. Cancellation captures the selected peer endpoint, not the
first configured host.

`HostBalance::ordered` is the default: preserve host order and the resolver's
address order. `HostBalance::random` shuffles the host traversal once per
connect/reset and shuffles each unpinned host's resolved addresses before trying
TCP. Resolve and exhaust one host's addresses before moving to the next host;
do not flatten all hosts into a single address lottery. Repeated host entries
remain distinct choices. Parsing/loading preserves the configured order and
per-host passwords; only execution randomizes traversal. A selected Connection
stays on its server, without balancing queries or replaying them elsewhere.

The generator is local to that connection attempt, seeded from OpenSSL before
network setup, with no global seed or scheduler-dependent shared state. Entropy
failure returns `io_error` before connecting. Prefer-standby's second pass reuses
the host permutation and resolves/shuffles addresses afresh. Random order does
not guarantee equal short-run distribution. See
[PostgreSQL's two-level host-balancing policy](https://www.postgresql.org/docs/18/libpq-connect.html#LIBPQ-CONNECT-LOAD-BALANCE-HOSTS).

The positive `connect_timeout` applies separately to each attempt, including
DNS, all TCP addresses for that host, startup and session checks. Unlike libpq's
per-address timeout accounting, this is one budget per configured host.
An outer `weave::timeout` can bound the whole
selection. Cancellation stops further attempts and drains pending I/O.
Unreachable destinations, attempt deadlines and unsuitable sessions allow the
next host. Authentication, TLS verification and protocol errors stop selection;
other errors after TCP connection also stop unless they are an attempt deadline.
There is no downgrade to plaintext or automatic replay after a query failure.

| TargetSession | Accepted session |
| --- | --- |
| `any` | First successful startup; no extra selection query |
| `read_write` | Not in recovery and default transactions are writable |
| `read_only` | Recovery or default transactions are read-only |
| `primary` | Not in recovery, even if default transactions are read-only |
| `standby` | In recovery |
| `prefer_standby` | First pass selects standby; second pass accepts any |

Checks use fully qualified PostgreSQL builtins and leave the session idle.
Selection is a connection-time snapshot, not a promise against later promotion,
configuration changes or application SQL. `server_options` is passed as the
startup `options` parameter; the server validates it and may reject settings.

## SCRAM Key Passthrough

This is a middleware integration feature, not a replacement for ordinary
application passwords. Each key is exactly 32 decoded bytes. Parse a padded
base64 value with `ScramKey::parse`, or construct `ScramKey` from a fixed-size
byte span:

```cpp
auto client_key = weave::pg::ScramKey::parse(client_key_base64);
auto server_key = weave::pg::ScramKey::parse(server_key_base64);
if (!client_key || !server_key)
  co_await weave::fail(std::errc::invalid_argument);

options.scram_client_key = std::move(*client_key);
options.scram_server_key = std::move(*server_key);
options.authentication.methods = {weave::pg::Authentication::scram_sha256};
auto connection = co_await weave::pg::connect(std::move(options));
```

Both keys bypass password derivation, including when no password is supplied.
A missing key is derived from the selected password using the server's salt and
iteration count. Client-key-only input without a nonempty password is rejected
before the initial SCRAM response: Weave cannot verify the server without its
key. A server key alone can accompany an ordinary password. These settings also
work in keyword/URI parsing and explicitly loaded service files; parsing does
not derive keys from a password or recover them from a live session.

Keys are salt/iteration-specific credentials. Supplying a key does not prove it
belongs to the current server or user; mismatched client keys fail server
authentication and mismatched server keys fail local proof verification.
Precomputed keys do not relax nonce validation, iteration bounds, authentication
restrictions, TLS verification or required channel binding. Restrict the method
explicitly when the connection must use SCRAM; keys do not themselves prohibit
other allowed methods.

`ScramKey` owns its inline bytes. Copies own independent credentials, moves clear
the source, and destruction clears the owned bytes. Successful startup also
discards the session's key copies; reset requires fresh credentials rather than
retaining reusable keys. The supplied raw/base64 input
and caller copies remain the application's responsibility. This is best-effort
cleansing, not locked memory or protection from crash dumps, swapping and all
compiler/provider temporaries.

Unlike the qualified libpq 18.4/18.6 controls, Weave supports a supplied client
key plus a password by deriving the missing server key. Those libpq builds
reject the valid server proof in that partial-key case. Neither path skips
server-proof verification. See
[qualification evidence](postgres-qualification.md#scram-key-passthrough).
PostgreSQL documents these middleware-specific options in its
[connection parameter reference](https://www.postgresql.org/docs/18/libpq-connect.html).

## Server Availability

`Task<ServerStatus> ping(Options)` probes PostgreSQL availability on its executing
Context; `Result<ServerStatus> ping_blocking(Options)` drives a calling-thread
Context outside another executing Context. Both use explicit Options, not hidden
environment or password-file loading.

```cpp
auto status = co_await weave::pg::ping(options);
```

`accepting` means a structurally valid initial authentication request or a server
SQL error other than `57P03` was received. `57P03` (cannot connect now) advances
to the next configured host. If none accepts, the final attempted host determines
`rejecting` versus `no_response`; a previous rejection does not mask a later
unreachable endpoint. `no_response` covers unreachable endpoints, ordinary
timeouts and transport loss without that evidence. Invalid configuration,
recognized certificate/protocol/resource failures and cancellation remain Task
or Result errors, rather than a fourth success value. A transport reset can
produce `no_response` instead of a decoded TLS alert.

The probe never answers a PostgreSQL authentication challenge, sends a query or
invokes an OAuth provider. Incorrect login/database values can still establish
availability, but Options must contain a syntactically valid nonempty user.
Configured TLS verification, mTLS identities and GSS transport protection still
apply; channel establishment may therefore use transport credentials. Target
session checks are not performed. Availability is not successful login, spare
connection capacity, writable-primary status or application-query health.

Trust rules and an authenticated GSS-encrypted transport can implicitly authorize
a session before the probe closes it; "no PostgreSQL challenge reply" does not
mean the server can never authorize a session. Conversely, a decoded HBA or
client-certificate rejection can still prove availability: the tested PostgreSQL
backend reports SQLSTATE `28000` for a missing required client certificate,
so the probe returns `accepting` even though that login is forbidden. Neither
result is an application authorization decision.

Probes retry connection refusal, ordinary timeout and transport loss with the
same configured transport policy. Recognized security/protocol/resource errors
remain terminal, as do connected timeouts during opted-in GSS negotiation.
This is a probe-only policy: ordinary connect/reset still do not retry connected
authentication/protocol failures. Native libpq controls stop on startup EOF;
Weave's unauthenticated probe can advance to an accepting second host.

Recovering-host traversal, independent TLS/mTLS and native libpq controls now
pass six Windows/Linux Debug, Release and ASan profiles. Permanent tests cover
both four-worker schedulers, both Windows IOCP layouts, cancellation and credential
cleanup. The selected regression, reduced-module and package gates pass too.
Dedicated PostgreSQL 18 controls now cover real authentication challenges, verified
TLS/mTLS, HBA denial, host recovery and Linux local-peer policy. Protected-GSS
controls cover an owned Linux Kerberos realm, both encryption policies, missing
credentials, incorrect services, captured default credentials, provider reuse,
and cancellation/deadline draining during native work and record protection.
Windows clients use a disposable WSL backend; this does not qualify Windows
domain/Kerberos deployment. See the
[qualification record](postgres-qualification.md#real-server-and-protected-gss-availability-2026-10-09).

## Reset

```cpp
co_await connection.reset(options);

// At a synchronous boundary:
auto status = blocking_connection.reset(options);
```

Reset closes the old transport and establishes a fresh session in the same
Connection object. Transactions, temporary tables, prepared statements, portals,
COPY state, queues, cached large-object function IDs and descriptors are discarded.
No SQL or transaction is replayed. Do not reuse borrowed metadata or old
CancelHandles as if they belonged to the replacement session.

Weave clears its stored password and SCRAM keys after authentication, so reset deliberately
requires fresh Options rather than retaining a reusable secret indefinitely.
Applications retaining their own Options remain responsible for that secret.
The connect overload taking `Diagnostic &` exposes server fields from startup
and any required target-session check; failed reset also updates `last_error()`
when the server supplied a diagnostic. Keep the borrowed Diagnostic alive until
the Task drains. A later protocol, transport or cancellation failure preserves
an earlier server diagnostic, but the returned error code still describes that
later failure. This is not a complete connection-attempt error history.

Invalid options, an active exchange, or an outstanding session Task (even one
not yet awaited) are rejected with an error without closing the current session.
Outstanding Tasks return `busy`; finish or destroy them before reset. Private
frame-owned borrow leases prevent reset from freeing a deferred Task's session;
there is no extra wrapper coroutine, shared allocation or retained old session.
Independent cancellation Tasks are the exception: they own immutable snapshots,
do not borrow the session and do not block reset. They still target the old backend.
Once the old transport closes, a failed or cancelled reset leaves it
closed; another explicit reset can recover it. The Connection and its execution
owner must outlive the Task. No concurrent exchange, move, destruction or nested
blocking event-loop drive is allowed.

## Cancellation

```cpp
auto cancel = connection.cancel_handle();
if (!cancel)
  co_await weave::fail(cancel.error());

co_await cancel->request();

// Or capture and send through the convenience method:
co_await connection.request_cancel();
```

Both request methods capture the selected backend's endpoint, process ID,
version-specific key and credential policy before their lazy Task starts. The
Task owns that snapshot even after the Connection, CancelHandle and original
Context are destroyed. It uses its executing Context for a new independent
connection, not the original query transport. Capture the handle on the
Connection's execution owner before sharing it across threads. Handle copies
share immutable state; requesting through a moved-from handle violates the
contract. `request_blocking()` drives a separate calling-thread Context and is
for callers outside an executing Context.

Reset does not refresh old handles or previously created request Tasks. Obtain
a fresh handle after reset to cancel the new session. Cancellation is backend-
scoped, not tied to a particular statement: a delayed request can race query
completion and subsequent commands. Successful dispatch and EOF mean neither
that the server acknowledged the request nor that a query was interrupted.
Await the original operation and inspect its result; cancellation does not
guarantee transaction rollback. Ordinary task-token or transport `cancel()`
instead makes an active exchange terminal.

The captured `connect_timeout` bounds the whole cancellation exchange, including
connect, SSL negotiation, handshake, request sending and waiting for EOF. Parent
cancellation also cancels and drains that exchange. Verified TLS/mTLS remains
verified on the new connection: refusal, invalid SSL replies and invalid
certificates fail rather than downgrade or send the cancellation secret. Local
connections recheck the original Linux kernel UID before sending the secret.

PostgreSQL cancellation has no response packet. Nonempty response data is a
protocol error. The TLS cancellation path accepts abrupt EOF specifically after
sending its complete packet and half-close because PostgreSQL can omit
`close_notify` on this one-shot connection. Ordinary TLS/query reads still
require authenticated EOF; this is not a general truncation bypass.

On Windows, that final TLS cancellation read also accepts `connection_reset`:
PostgreSQL's expected connection closure can surface as `WSAECONNRESET` instead
of EOF. This applies only after the complete cancellation packet and TLS send
shutdown succeed. Handshake, certificate, write, deadline, task cancellation and
nonempty response failures remain errors. Ordinary TLS reads are unchanged.
The [real-server and forced-reset controls](postgres-qualification.md#windows-certificate-policy-server-controls-2026-10-10)
cover this boundary; dispatch still does not prove a query was interrupted.

## Scope

Verified TLS remains the default. Randomized host balancing and native local
sockets and explicit Linux username-policy loading are implemented. Windows peer
credentials and some deployment/authentication qualification remain pending. This is an explicit
supported subset, not drop-in libpq connection-string compatibility.
See the [parity matrix](postgres-parity.md).

Connection selection/reset semantics reference the
[PostgreSQL 18 connection-control manual](https://www.postgresql.org/docs/18/libpq-connect.html).
Weave deliberately differs by enforcing verified TLS, finite positive attempt
timeouts and explicit reset credentials.
