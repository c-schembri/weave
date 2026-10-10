# PostgreSQL Passwords

All password APIs take the target **user first, password second**. The module
uses OpenSSL Crypto and ICU, not libpq, to produce PostgreSQL-compatible
SCRAM-SHA-256 and explicitly selected legacy MD5 verifiers.

## Change a Password

On an existing connection:

```cpp
co_await database.change_password("app", new_password);
```

The Task returns the server's `ResultSet`, with command tag `ALTER ROLE` on
success. `BlockingConnection::change_password` returns `Result<ResultSet>` and
drives the same implementation on its owned Context.

By default the operation reads `SHOW password_encryption`. Automatic MD5
selection is refused unless explicitly allowed:

```cpp
co_await database.change_password("app", new_password, {
  .allow_md5 = true,
});
```

Prefer an explicit SCRAM policy when no server-policy lookup is wanted:

```cpp
co_await database.change_password("app", new_password, {
  .algorithm = weave::pg::PasswordAlgorithm::scram_sha256,
  .iterations = 8192,
});
```

Explicitly selecting `PasswordAlgorithm::md5` is itself a legacy opt-in.
Password-storage policy is separate from `Options::allow_md5_password`, which
controls connection authentication. Neither setting enables the other.

One session lease covers policy lookup and mutation. Other commands and reset
cannot interleave. The username is validated and quoted in the current client
encoding **after** lookup, including any intervening ParameterStatus update.
The SQL contains only the generated verifier, never the original password.
The typed password-change request and response bodies remain redacted even
with application-payload protocol tracing enabled. This does not sanitize
application-owned SQL, server diagnostics or notice/lifecycle callbacks.

Use verified TLS or encrypted GSS for remote administration. An explicitly
plaintext connection remains plaintext; hashing is not transport security.

## Generate a Verifier

The synchronous utility requires no connection and returns `Result<std::string>`:

```cpp
auto verifier = weave::pg::password_verifier("app", new_password);
```

It defaults to SCRAM-SHA-256, a fresh 16-byte salt and 4,096 iterations. Pass
an algorithm and iteration count explicitly to override those defaults. MD5
uses the supplied username; SCRAM does not incorporate the username.

The connection version can instead consult server policy:

```cpp
auto verifier = co_await database.password_verifier("app", new_password);
```

It accepts the same `PasswordOptions` as password changes. Its blocking-facade
equivalent returns `Result<std::string>`. These member methods still require
an idle, open connection, including when an explicit algorithm avoids lookup.
Use the free utility for independent CPU-only work.

The returned verifier is an ordinary caller-owned string. Treat it as sensitive;
it is not automatically cleansed on destruction. Avoid manually constructing
password-change SQL when the typed operation is available.

## Ownership and Failure

- Both asynchronous member factories copy and validate the password before
  initial suspension. The caller may release or modify that input afterward.
  The Task owns its username and borrows its Connection through completion.
- Internal password, verifier, key and mutation-request storage is best-effort
  cleansed, including dropped/rejected unstarted Tasks. This does not erase
  caller copies, all allocator history or third-party temporaries.
- Inputs are bounded to 65,536 bytes each and reject embedded NUL. Iterations
  must be between 1 and 1,000,000. SCRAM applies SASLprep, with PostgreSQL's
  raw-byte fallback for invalid/prohibited/unassigned/bidirectional inputs.
- Policy lookup and mutation perform asynchronous transport work, but password
  hashing is synchronous CPU work on the current executor. A large iteration
  count can delay cancellation and other work; Task deadlines do not preempt it.
- Fully drained SQL errors preserve SQLSTATE and allow connection reuse. Invalid
  policy-result shapes are protocol failures and make the connection terminal.
  Unsupported policy/encoding or a request-size rejection before mutation leaves
  the drained connection open.
- Cancellation during an incomplete exchange closes and drains the connection.
  Cancellation or disconnection after sending ALTER does **not** prove that the
  password remained unchanged. There is no automatic retry or rollback.

## Compatibility and Status

The reference capabilities are
[PQencryptPasswordConn and PQchangePassword](https://www.postgresql.org/docs/18/libpq-misc.html).
Weave deliberately uses consistent user/password argument order, typed
algorithm selection, an explicit MD5 opt-in and per-operation iteration counts.
The iteration default is Weave's 4,096, not discovery of a server hashing-cost
setting. Generation does not reproduce libpq's C allocation/pointer APIs.

Windows/Linux Debug, Release and ASan controls pass real PostgreSQL
18 password changes/reconnects, independent libpq controls, Unicode verifier
derivations, protocol-shape/encoding failures, trace redaction and cancellation
with observed native pending/drained counts. Four-worker tests cover 64 sessions
for both schedulers. Permanent regression, affected shared-engine, reduced-module
and packaging gates pass: 386 CTest entries plus native Windows and GSS-disabled
real-server controls. This is not full release or deployment qualification.
See the [evidence record](postgres-qualification.md#password-regression-qualification-2026-10-08).
