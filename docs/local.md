# Local Sockets

Include `<weave/local.hpp>` and link `weave::local`. It requires Core and IO,
not TCP, Runtime, TLS or PostgreSQL. Both Windows IOCP and Linux io_uring drive
operations through the same internal socket completion machinery as TCP.

## Connect And Listen

```cpp
auto client = co_await weave::local::connect("/run/my-app/stream.sock");
auto listener = co_await weave::local::listen("/run/my-app/stream.sock", 512);
auto accepted = co_await listener.accept();
```

Windows paths are UTF8, for example `"C:/my-app/stream.sock"`; the parent directory
must already exist. Linux also accepts abstract names such as `"@my-app"`.
Arguments are owned by the lazy setup Tasks, not borrowed string views. Contextless
setup selects the executing Context when awaited. Explicit Context overloads
are also available; `local::listen(ctx, address, backlog)` is synchronous and
returns `Result<LocalListener>`. Backlog must be positive.

`LocalStream` provides `read()`, `read_exactly()`, `write_all()` and synchronous
`shutdown_send()`, `cancel()` and `close()`. Reads return zero at EOF; premature
EOF in `read_exactly()` reports `connection_reset`. Empty operations complete
without native submission, but still require a compatible execution scope.
One read and one write may be outstanding simultaneously; overlapping operations
in the same direction and close during active I/O report `operation_in_progress`.
Moves require no active operation.

Sockets retain their original Context across work-stealing task migration. Keep
the Context, socket and buffers alive until cancellation completes and operations
are joined. Cancellation requests do not destroy active frames. See
[execution/lifetime rules](context.md) and [cancellation](cancellation.md).

## Names And Peer Credentials

`stream.local_address()` and `stream.peer_address()` synchronously return
`Result<std::string>`. Unnamed peers have an empty name. Linux abstract names
use the `@` spelling. `listener.local_address()` returns its cached address by
const reference, including after close. Inspect stream names before closing.

Linux `stream.peer_credentials()` returns `Result<LocalPeer>` containing native
process, user and group IDs obtained through `SO_PEERCRED`. These are kernel
credentials, not user-supplied startup data. Translating a UID to an account
name is a separate policy/resolution step; this API does not perform NSS lookups.
Windows returns `operation_not_supported`, not invented credentials or SID-to-UID
translations. Peer IDs do not imply that the application protocol is authorized.

## Filesystem Ownership

Weave never unlinks a socket path, before bind or after close. A bind failure
does not remove an existing file. The caller owns the private directory,
permissions/ACLs, stale-socket policy and cleanup after every borrower has drained.
Do not blindly delete a socket path in a directory writable by untrusted users.
Linux abstract sockets disappear when the last bound handle closes and do not
use filesystem permissions. Windows pathname sockets use native filesystem
access controls. [Microsoft's AF_UNIX overview](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/).

## Current Limits

Only byte-stream sockets are supported: no datagrams, ancillary descriptor
passing, arbitrary embedded-NUL names, or socket-pair helper. Filesystem names
are limited to 107 UTF8 bytes; Linux abstract names to 107 bytes after `@`.
Overlong names fail before socket creation.

Windows abstract listen/connect currently report `operation_not_supported`.
On the tested Windows 11 / SDK 10.0.26100 system, independent raw Winsock
ConnectEx controls rejected abstract destinations with WSAEINVAL while filesystem
controls completed through IOCP. A separate bound blocking-connect diagnostic
also rejected the abstract destination. This is observed behavior on this host,
not a claim that all Windows releases lack native abstract support. No blocking
fallback, polling or helper thread has been introduced into Weave.

PostgreSQL uses this transport for explicit socket-directory hosts and same-address
cancellation connections. It can enforce a Linux UID before startup and rechecks
the peer UID before cancellation secrets. Username-based `requirepeer` remains
pending. See [PostgreSQL local sockets](postgres-connections.md#local-sockets) and
the [parity checklist](postgres-parity.md).

## Validation, 2026-10-07

| Gate | Windows | Linux/WSL2 |
| --- | --- | --- |
| Full non-packaging correctness suite, Debug | 22/22 | 24/24 |
| Full non-packaging correctness suite, Release | 22/22 | 24/24 |
| Full non-packaging correctness suite, ASan | 22/22 | 24/24 |
| Isolated Local/TCP and combined relocated packaging | 3/3 | 3/3 |
| Supplemental Local ASan process repetitions | 20/20 | 20/20 |

The Local suite checks address bounds, lazy owned arguments, existing-file
preservation, cached names, EOF, half-close, 128 KiB transfers, short exact reads,
pending accept cancellation/reuse, read cancellation, overlapping reads, active
close rejection and closed-handle failures. Runtime cases use four workers,
16 concurrent sessions per configuration, both schedulers and both successful
completion modes. Windows covers sharded/shared IOCP; Linux covers sharded rings
and both filesystem/abstract address forms in Context tests.

Toolchains: Windows MSVC 19.44 / SDK 10.0.26100, and WSL2 Ubuntu GCC 15.2 /
liburing 2.14. The shared-awaiters extraction also passed existing TCP, IO,
Runtime, Sync, TLS and PostgreSQL gates. No timings or performance comparisons
were rerun for this new API and organizational extraction.

An earlier Windows Debug run had one failure in the existing TLS ownership
fixture. Its error code was not captured; the exact cause is unknown. Diagnostics
confirmed an avoidable IPv6-refusal delay in that IPv4-only fixture, and the
confirmed dual-stack fixture now serves both localhost families and reports
error details. See [the retained qualification limits](tls-release.md#local-socket-requalification).
The table above reports complete final runs, not a selective retry of failed
entries. Raw failed runs, native controls and passed matrices remain in ignored
qualification output; they are not tracked benchmark results or a security audit.
