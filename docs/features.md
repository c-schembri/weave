# Weave Features

## Implemented

| Feature | Versions / Minimum Requirements |
| --- | --- |
| Coroutine tasks and error handling | C++23; exceptions disabled |
| Context and multicore runtime | Windows IOCP or Linux io_uring |
| Structured concurrency, cancellation and timers | Context or Runtime |
| TCP clients/servers and asynchronous DNS | IPv4 and IPv6 |
| Local sockets | AF_UNIX |
| TLS and mutual TLS | TLS 1.2/1.3; OpenSSL 3.5+ |
| Bounded channels and semaphores | Context or Runtime |
| Native PostgreSQL client | Wire protocol 3.0/3.2; OpenSSL 3.5+; ICU 70+; server minimum TBD |
| Native MySQL client | Classic protocol; OpenSSL 3.5+; server minimum TBD |
| Database row/struct mapping | C++23; Core only |
| Database connection pooling | PostgreSQL or MySQL driver |
| PostgreSQL schema migrations | PostgreSQL 10+; UTF-8 |
| MySQL schema migrations | MySQL driver; server minimum TBD |
| JSON serialization/deserialization | Bundled Glaze 9.0.0 |
| Modular CMake packaging | CMake 3.25+ |
| Windows IOCP backend | Windows x64; Visual Studio 2022 C++ tools |
| Linux io_uring backend | Linux 5.11+; GCC 14+; glibc; liburing 2.3+; io_uring enabled |
| Tests and automation | Python 3.11+ |

## Planned

| Feature | Target Version / Dependencies |
| --- | --- |
| Native Kafka producer/consumer with batching | Modern broker minimum TBD; TCP/TLS |
| Native HTTP/1.1 client/server | HTTP/1.1; TCP/TLS |
| Native HTTP/2 client/server | HTTP/2; TCP/TLS |
| Native HTTP/3 client/server | HTTP/3; QUIC |
| UDP | IOCP/io_uring |
| Native QUIC | Version TBD; UDP and QUIC-specific TLS integration |
| WebSocket clients/servers | TCP/TLS; HTTP handshake integration |
| Native Redis client | Server/RESP versions TBD; TCP/TLS |
| gRPC | HTTP/2; serialization version TBD |
| Blocking-work executor | Bounded worker pool; Task/executor integration |
| Asynchronous file I/O | Platform file backends |
| Consistent metrics and tracing | Existing runtime/protocol hooks |
| Linux CI | Linux build requirements above |
| Protocol fuzzing and interoperability testing | Protocol-specific peers and harnesses |
| API stability and versioning policy | Policy TBD |
| Project license | License TBD |
