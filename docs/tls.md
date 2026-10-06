# TLS

**Experimental, not production-ready.** Link optional `weave::tls` and include
`<weave/tls.hpp>`. It uses OpenSSL 3's TLS engine over ordinary asynchronous stream
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

Client verification is mandatory. DNS names verify the certificate hostname and
send SNI; IP literals verify IP subject-alternative names. There is no insecure
verification-off option. `TlsContext::client({.ca_file = "issuer.pem"})` uses an
explicit trust file instead of defaults. By default OpenSSL paths and Windows
ROOT certificates are loaded. Importing ROOT is not equivalent to the complete
Windows certificate-chain policy: distrust, revocation and enterprise policy are
not mirrored. OCSP/CRL checks are not configured in this first implementation.

TLS 1.2 and 1.3 are enabled; older protocols are unavailable. Set `min_version`
and `max_version` to constrain negotiation, and `.alpn = {"h2", "http/1.1"}`
to offer application protocols. Server preference determines selection when both
sides offer ALPN; incompatible offers fail. ALPN negotiates a label, not an HTTP
implementation. `version()` and `negotiated_protocol()` are synchronous queries.

## Server And Adapters

```cpp
auto credentials = weave::TlsContext::server({
  .certificate_file = "chain.pem",
  .private_key_file = "key.pem"
});
```

The synchronous factory validates the PEM chain and matching key. Encrypted keys
can use `private_key_password`; loading never prompts on stdin. The credential
object is cheaply copyable and shareable across workers.

Inside a handler, upgrade an owned transport:

```cpp
auto client = co_await weave::tls::server(std::move(transport), credentials);
```

`tls::client(transport, credentials, server_name)` performs the corresponding
verified client handshake. Both adapt any nothrow-movable `CancellableStream` and
return `TlsStream<S>` only after the handshake completes. Keep the credentials
alive until setup finishes; each established stream retains its native credentials.
No backend socket types or OpenSSL headers enter the public API.

See the [concurrent TLS echo example](../modules/tls/examples/echo/README.md).

## I/O, Shutdown And Cancellation

One reader and one writer may overlap. Competing operations in the same direction,
or shutdown while I/O is active, return `operation_in_progress`. This does not
make a stream usable from arbitrary threads or unrelated Contexts. SSL engine
calls are serialized; underlying transport reads/writes retain their usual affinity.

`read` returns zero only for authenticated TLS `close_notify`. Abrupt transport
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

This first pass has no mTLS API, session-cache controls, 0-RTT, certificate hot
reload, or revocation integration. Do not infer those features from OpenSSL's
capabilities or treat the current tests as a security audit.

## Build And Validation

Install OpenSSL 3 separately (for example `vcpkg install openssl:x64-windows`), then:

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

Tests generate temporary CA/leaf credentials at execution, not committed private
keys. They exercise TLS versions, verified names/IPs, rejected chains/expiry/ALPN,
fragmentation, full duplex, truncation, cancellation, and four-worker configurations.
Python's independent TLS adapter checks both client/server roles on loopback.
Correctness CI runs these tests without benchmarks or external network peers.

Engine rules follow OpenSSL's
[error handling](https://docs.openssl.org/3.6/man3/SSL_get_error/),
[write retries](https://docs.openssl.org/3.6/man3/SSL_write/), and
[shutdown](https://docs.openssl.org/3.6/man3/SSL_shutdown/) contracts.
