# PostgreSQL OAuth work

Custom-provider and native HTTPS device-authorization authentication are
implemented. Native device flow has independent synthetic HTTPS controls and an
opt-in real Keycloak/PostgreSQL deployment gate. This is not full libpq OAuth
parity, arbitrary-IdP compatibility or a production/security-audit claim.

## Connection Setup

Configure an application-trusted issuer and an owning asynchronous provider:

```cpp
auto provider = weave::pg::OAuthProvider::create(acquire_token);
if (!provider)
  return weave::report_error(provider.error());

auto options = weave::pg::Options::parse(
  "host=db.example user=app dbname=app require_auth=oauth "
  "oauth_issuer=https://identity.example/tenant oauth_client_id=app");
if (!options)
  return weave::report_error(options.error());

options->oauth->provider = *provider;
// Inside a Task, with an executing Context or Runtime:
auto connection = co_await weave::pg::connect(std::move(*options));
```

`acquire_token` takes an owning `OAuthRequest` and returns `Task<OAuthToken>`;
the provider callable must be const-invocable and `noexcept`. It owns the token
acquisition protocol and its trust policy. Weave does not fetch the supplied
discovery URL for it or verify the token's JWT claims; PostgreSQL's validator
decides whether the token authorizes the requested role.

`Options::oauth` also supports typed `OAuthOptions`. Keyword/URI parsing and
explicit service loading support `oauth_issuer`, `oauth_client_id`,
`oauth_client_secret`, and `oauth_scope`. Parsing is synchronous and side-effect-free;
it never instantiates a provider. Partial settings can be completed before connecting. Connect validates
the full issuer/client/scope configuration and requires a provider before I/O.
Client secrets become immutable, owning snapshots in `OAuthOptions::client_secret`.
An empty parsed secret supplies no connection credential; an explicit empty
connection value overrides a service-file value. Connection/service decoding and
file buffers cleanse owned credential bytes on replacement, growth and failure.
There are no invented OAuth environment defaults; libpq 18 does not define
environment mappings for these fields either.

An absent scope uses the server's discovered scopes. An explicitly configured
scope, including the empty string, overrides them. Issuers use a bounded HTTPS
identifier subset without userinfo, query, fragment or normalization. Host/port
spelling is compared byte-for-byte, not canonicalized. `issuer` may instead name
an explicit `/.well-known/openid-configuration` or
`/.well-known/oauth-authorization-server` URL. Prefix and postfix forms derive
the literal issuer identity; middle-position and ambiguous forms fail. This pins
the exact discovery URL, including when the server omits its URL. A different
server-provided URL fails even if it would otherwise be bound to the same issuer.

## Cached Tokens

An optional synchronous lookup can avoid the discovery connection:

```cpp
weave::Result<std::optional<weave::pg::OAuthToken>> find_cached_token(
  const weave::pg::OAuthRequest &request) noexcept;

auto provider = weave::pg::OAuthProvider::create(acquire_token, find_cached_token);
// Or pass the lookup as the third argument to OAuthProvider::device(...).
options->oauth->provider = *provider;
options->oauth->issuer = "https://identity.example/tenant/.well-known/openid-configuration";
options->oauth->scope = "read write";
```

The callback must be memory-only and nonblocking. A present token is a hit, an
empty optional is a miss, and an error terminates connection startup. Weave calls
it only for an explicit discovery URL, after verified TLS or negotiated GSS
encryption and an allowed OAUTHBEARER challenge. It receives the derived literal
issuer, pinned discovery URL, selected database/user/endpoint, client ID and
optional client-secret owner. With an absent configured scope, `scope` is empty
and `scope_explicit` is false; explicit empty scope also has empty text but sets
the flag. Configure scopes when possible, or deliberately handle their absence.

A hit authenticates on the existing protected connection. A miss preserves the
empty-token discovery, closed socket, asynchronous acquisition and pinned
reconnect. Token rejection never triggers implicit refresh, a second lookup or
host failover. Cancellation is rechecked after lookup and before sending a token.
`provider.cached_token(request)` is also a synchronous operation for explicit
application use; it does not contact PostgreSQL or perform issuer discovery.

Applications own cache keys, expiry, revocation and thread-safe access. Weave does
not create a global cache, save returned tokens, refresh them or validate their
JWT claims. Provider copies may call the same lookup concurrently. The callable
is retained through invocation, and each returned `OAuthToken` owns its cleansing
storage. Ordinary caller-created token copies are still the caller's responsibility.

## Native Device Authorization

Provide an application-owned prompt handler instead of implementing token HTTP:

```cpp
weave::Task<void> show_authorization_prompt(weave::pg::OAuthDevicePrompt prompt);

auto provider = weave::pg::OAuthProvider::device(show_authorization_prompt);
if (!provider)
  return weave::report_error(provider.error());

options->oauth->provider = *provider;
auto connection = co_await weave::pg::connect(std::move(*options));
```

The handler takes a move-only, owning prompt by value. Show `verification_uri()`
and `user_code()` together; `verification_uri_complete()` is an optional browser
convenience, not a replacement for showing the code. URI fragments are preserved
for display/browser use but never requested by the token transport. Borrowed
views last only as long as their prompt owner. The private device code is never
exposed. `expires_at()` is a monotonic deadline; failure/cancellation of the handler
stops acquisition and drains its task. Weave does not open a browser or print
credentials implicitly. Applications own their UI, escaping and user confirmation.

For confidential clients, configure the provider explicitly:

```cpp
auto secret = weave::pg::OAuthClientSecret::parse(client_secret);
if (!secret)
  return weave::report_error(secret.error());

auto provider = weave::pg::OAuthProvider::device(
  show_authorization_prompt,
  {.client_secret = std::move(*secret)});
```

`OAuthClientSecret` accepts nonempty, NUL-free UTF8 up to 64 KiB and owns cleansing
storage. Copies share one immutable allocation; the last owner cleanses it.
Moves leave an empty source. Its `value()` is borrowed and caller-made ordinary
string copies are not cleansed. `Options`, `OAuthRequest` and lazy connect/reset
Tasks retain their own snapshots; selected sessions discard OAuth configuration
after successful startup. `OAuthDeviceOptions::client_auth` selects `automatic`,
`none`, `client_secret_basic`, or `client_secret_post`. Automatic public clients use no
secret; with a secret, Basic is preferred when advertised (and is the metadata
default), then POST if advertised. Explicit confidential methods must be
advertised. Client registration remains the authorization server's responsibility;
there is no fallback to another method after authentication failure.
Basic form-encodes both credentials before base64; POST uses the form body.
The native provider uses a request/connection secret before its optional factory
default. An empty connection secret does not suppress that programmatic default.
Explicit Basic/POST factories may receive their secret through the request;
missing confidential credentials and `none` combined with a secret fail before
metadata I/O. A custom provider receives the owning snapshot through
`OAuthRequest::client_secret` and decides how to use it.
Only one authentication method is sent. Encoded requests still obey transport
bounds, including a 128 KiB Basic-header cap; large credentials can exceed these
bounds even when their decoded text passes parsing.

The provider's optional `tls` is an independent HTTPS trust/credential snapshot;
it is not the PostgreSQL TLS context. By default it uses system trust and
HTTP/1.1 ALPN. A custom snapshot can use the TLS module's CA, mTLS and session
policies. Do not reuse a PostgreSQL-only ALPN configuration for HTTPS or an
HTTP-only ALPN configuration for PostgreSQL.

Before fetching metadata, Weave validates the configured issuer and derived
well-known URL. Returned issuer identity must match literally; device/token
endpoints must be bounded HTTPS URLs and the device grant must be advertised.
Endpoints may use different hosts when authorized by that issuer's trusted
metadata. There are no implicit redirects, browser requests, decompression or
JWT validation; PostgreSQL's validator authorizes the resulting bearer token.
Client IDs, scopes and user codes use the supported ASCII subset; user codes are
bounded to 4 KiB and verification URLs to 8 KiB. Unicode URI text must already be
represented through the supported ASCII/percent-encoded URL syntax. Refresh
tokens, DPoP and automatic token caching are not implemented.

The first token request waits the advertised interval (default five seconds;
zero is clamped to one). `authorization_pending` waits again; `slow_down`
permanently adds five seconds; a request timeout doubles the interval. Other
transport/protocol/authentication errors stop instead of retrying. Poll intervals
and device-code lifetimes are bounded to 24 hours. Request latency conservatively
counts against code expiry; expired grants are not prompted or accepted.
`OAuthDeviceOptions::request_timeout` bounds each HTTP exchange (default 30
seconds). Device expiry also covers prompt work and polling; connection-level
`acquisition_timeout` covers the complete acquisition. These deadlines cancel and
drain cooperatively, not forcibly preempt application code.

## Authentication Flow

Weave selects OAUTHBEARER only when advertised, configured and allowed by the
authentication policy. `channel_binding=require` cannot be satisfied by OAuth.
Verified TLS or actually negotiated GSS encryption is required before any OAuth
response; plaintext and local-socket OAuth are unsupported.

Unless an explicit-discovery cache lookup hits, the first protected connection
sends an empty OAUTHBEARER response to request metadata. Strict, bounded yyjson
parsing rejects invalid UTF8, trailing bytes,
duplicate decoded keys, invalid field types, oversized/deep structures and
retargeted discovery URLs. Only configured-issuer-derived OIDC/OAuth well-known
URLs, or the exact explicitly configured discovery URL, are accepted. An
authenticated success on this discovery exchange is an
error, not a credential-free login.

After the dummy rejection response and server error, Weave closes the discovery
connection before invoking the provider. Acquisition has its own cooperative
`OAuthOptions::acquisition_timeout` (30 minutes by default); each TCP/TLS/startup
attempt uses `connect_timeout`. A timeout cancels and drains, not forcibly preempts
an uncooperative provider. Reconnection is pinned to the selected numeric address,
port, hostname-verification/credential snapshot and protected transport policy.
OAuth rejection, provider failure, post-connect timeout, invalid framing or method
switching never retries another host or refreshes credentials in a loop.

The provider is released from the selected live session after authentication;
reset takes fresh Options and follows the same flow. The Task and blocking facades
share this native implementation. Providers may perform asynchronous work on the
same execution graph; no authentication helper threads are introduced.

## Implemented foundation

`<weave/postgres/oauth.hpp>` provides:

- `OAuthToken::parse(text)`: a move-only credential owner. It validates the complete
  RFC 6750 bearer-token grammar and a 64 KiB bound before allocating. Owned token
  storage is cleansed on replacement/destruction. A moved-from token is empty.
- `OAuthRequest`: owning issuer, client ID, scope, discovery URL, host, port, user
  and database values, plus an optional immutable client-secret owner for a
  provider invocation.
- `OAuthProvider::create(factory)`: a copyable owner of one move-only,
  const-invocable `noexcept` callable returning `Task<OAuthToken>`.
- `provider.request(request)`: a lazy Task that owns the request and retains the
  callable before initial suspension, including a coroutine-lambda closure.
  Empty factories and overlong/NUL-containing request text fail explicitly;
  cancellation already requested before execution does not invoke the factory.

Providers run on the caller's task execution graph, not a helper thread. Direct
children inherit cooperative cancellation and must drain before the parent owner
is released. Provider copies may execute concurrently: const invocation does not
make mutable captured state thread-safe. Use asynchronous Weave operations, not
blocking network calls or an uncooperative CPU loop. A moved-from provider must
not be invoked; that is a contract violation.

`OAuthToken::value()` is a borrowed credential view. Do not log it. Copies made
by callers are their responsibility; cleansing is not locked memory or a promise
to erase every compiler, allocator or third-party temporary.

The private SASL exchange produces the PostgreSQL OAUTHBEARER initial response
in cleansing storage, validates mechanism-list termination and bounds, and
permits either direct authentication success or one dummy rejection response.
A rejected exchange can never subsequently become successful. Connection startup
uses this codec and the private strict JSON integration. Decoded metadata is held
in a bounded cleansing pool; this does not erase caller-owned response copies.

The private HTTPS foundation uses Weave TCP/TLS, pinned llhttp 9.4.3 for HTTP
framing, and uriparser 1.0.2 for literal RFC 3986 URL parsing. It is not a public
HTTP client or an automatically selected token provider. HTTPS URLs reject
userinfo, fragments, invalid ports, ambiguous legacy IPv4 spellings and invalid
DNS labels. Encoded path/query and authority spelling are preserved; configured
issuer identifiers still forbid queries and use byte-identical trust comparisons.
The same URI parser now validates configured issuer identifiers.

Requests own cleansing form/authorization storage before initial suspension.
The exchange enforces TLS trust/hostname verification, HTTP/1.x ALPN, a cooperative
deadline, 32 KiB parsed headers, 64 KiB bodies and 256 KiB total response bytes.
Strict framing accepts content-length, chunked and authenticated TLS EOF responses;
ambiguous framing, unsupported transfer/content encodings and trailers fail.
It never follows redirects or decompresses bodies. Returning an HTTP status does
not authorize an endpoint or accept a token: discovery/device policy must still
validate status, JSON, issuer and grant semantics. Cancellation drains transport
operations before releasing frames and owned credentials.
Each exchange uses a fresh connection. Trailing bytes already received alongside
a framed reply fail; later unread records are discarded by closing, not inspected
or reused for another response.

## Real Keycloak Gate

`WEAVE_POSTGRES_KEYCLOAK_TESTS=ON` enables the Linux-only
`weave_postgres_oauth_keycloak` release gate. It is off by default, including normal
CI and library-only builds. Nothing is downloaded or installed by configuring or
running it. Prepare the pinned archives in `WEAVE_POSTGRES_KEYCLOAK_TOOLS`:

- [Keycloak 26.8.0](https://github.com/keycloak/keycloak/releases/tag/26.8.0),
  `keycloak-26.8.0.tar.gz`.
- [Temurin 21.0.12.1+1](https://github.com/adoptium/temurin21-binaries/releases/tag/jdk-21.0.12.1%2B1),
  `OpenJDK21U-jre_x64_linux_hotspot_21.0.12.1_1.tar.gz`.

The driver verifies the pinned SHA-256 digests and safely extracts fresh trees
for every run, rather than trusting a mutable cached deployment. It requires
Python with tarfile's `data` extraction filter (Python 3.12+ or a supported
backport), PostgreSQL 18 server binaries/headers, OpenSSL's command-line verifier,
and private libcurl/json-c development files for the server control.

With those dependencies available:

```sh
cmake -S . -B build/keycloak -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  '-DWEAVE_MODULES=postgres;runtime' -DWEAVE_BUILD_TESTS=ON \
  -DWEAVE_POSTGRES_OAUTH_SERVER_TESTS=ON \
  -DWEAVE_POSTGRES_SERVER_BIN=/usr/lib/postgresql/18/bin \
  -DWEAVE_POSTGRES_OAUTH_SERVER_INCLUDE=/usr/include/postgresql/18/server \
  -DWEAVE_POSTGRES_KEYCLOAK_TESTS=ON \
  -DWEAVE_POSTGRES_KEYCLOAK_TOOLS=/path/to/verified/archives
cmake --build build/keycloak --target weave_postgres_oauth_keycloak --parallel 8
ctest --test-dir build/keycloak -R '^weave_postgres_oauth_keycloak$' \
  --no-tests=error --output-on-failure -j 1
```

This starts owned, loopback-only services with fresh credentials. Keycloak uses
its production profile, HTTPS only and a PostgreSQL datastore reached with
`sslmode=verify-full`. An independent HTTPS user agent follows the real device
login/approval forms; no password grant substitutes for the device flow. The
shared certificate fixture supplies Subject/Authority Key Identifiers, and the
driver also verifies its chain with `openssl verify -x509_strict`.

Weave drives its native HTTPS provider and verified PostgreSQL connection.
The private server extension asks Keycloak's authenticated, certificate-verified
introspection endpoint to validate signed access tokens; it additionally enforces
issuer, client, role, audience, scopes and expiry. **Weave does not verify JWTs.**
The extension is a qualification control, not a production validator product.

Controls cover client-secret Basic/Post (including reserved secret characters),
Context and blocking connect/reset, eight concurrent sessions on each four-worker
scheduler, real cached tokens, modified signatures, missing audience/scope,
wrong role, expiry, bad client credentials, untrusted IdP certificates, user
denial, pending-grant timeout and subsequent reuse. Removing Runtime retains the
Context/blocking and rejection controls. Temporary realms, keys, databases and
process groups are drained/deleted; run evidence remains in the ignored build
directory. This is not a browser UX, revocation/key-rotation/refresh-token soak,
Windows real-IdP qualification, FIPS qualification or independent security review.

## Remaining integration

Remaining work before full OAuth qualification/parity:

- Keep provider/UI configuration explicit; add the remaining libpq flow and
  deployment controls without inventing a default interactive UI.
- Complete cache/discovery deployment controls with equivalent issuer trust rules;
  expiry and revocation remain application policy rather than an implicit cache.
- Expand qualification beyond the pinned Keycloak deployment. Linux's opt-in combined
  native GSS/OAuth controls cover cached/custom/device acquisition, blocking/reset,
  both Runtime schedulers, downgrade rejection and cancellation. They verify
  backend encryption without TLS and keep OAuth distinct from GSS authentication.
  Windows domain/GSS interoperability remains separate. Those combined controls
  use an opaque-token validator and synthetic HTTPS IdP; the separate Keycloak
  gate uses actual signed tokens, real device forms and server introspection.
  Neither substitutes for combined real-IdP/GSS deployment qualification.
- Complete deployment/release validation and matched OAuth performance workloads.
  Existing functional libpq controls are not an OAuth benchmark.

Reference: [PostgreSQL OAUTHBEARER exchange](https://www.postgresql.org/docs/18/sasl-authentication.html#SASL-OAUTHBEARER),
[libpq OAuth flow](https://www.postgresql.org/docs/18/libpq-oauth.html),
[RFC 7628](https://www.rfc-editor.org/rfc/rfc7628.html),
[RFC 6750 token grammar](https://www.rfc-editor.org/rfc/rfc6750.html#section-2.1),
[RFC 8628 device flow](https://www.rfc-editor.org/rfc/rfc8628.html),
[RFC 8414 metadata](https://www.rfc-editor.org/rfc/rfc8414.html),
[RFC 6749 client authentication](https://www.rfc-editor.org/rfc/rfc6749.html#section-2.3).
