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
| GraphQL client | Specification revision TBD; HTTP/JSON |
| GraphQL server | Schema validation, resolvers and execution; specification revision TBD |
| GraphQL subscriptions | Transport/version TBD; pub/sub integration |
| SQLite | Minimum TBD; embedded engine; blocking-work executor for async use |
| SQL Server | TDS/server minimums TBD |
| MariaDB compatibility | MySQL driver qualification; server minimum TBD |
| Oracle Database | Server minimum and driver approach TBD |
| IBM Db2 | Server minimum and driver approach TBD |
| CockroachDB compatibility | PostgreSQL driver qualification; server minimum TBD |
| YugabyteDB compatibility | PostgreSQL driver qualification; server minimum TBD |
| Other PostgreSQL-compatible services | Per-vendor qualification; minimums TBD |
| MongoDB | BSON and OP_MSG; server minimum TBD |
| Couchbase | Server/protocol minimums TBD |
| Cassandra | CQL; server/protocol minimums TBD |
| ScyllaDB compatibility | CQL driver qualification; server minimum TBD |
| DynamoDB | API version TBD; HTTPS |
| Memcached | Server/protocol minimums TBD |
| Neo4j | Server/protocol minimums TBD |
| Elasticsearch | Server minimum TBD; HTTP/JSON |
| OpenSearch | Server minimum TBD; HTTP/JSON |
| ClickHouse | Server minimum and native/HTTP interface TBD |
| DuckDB | Minimum TBD; embedded engine; blocking-work executor for async use |
| NATS and JetStream | Server minimum TBD; TCP/TLS |
| RabbitMQ compatibility | AMQP 0.9.1 and AMQP 1.0 qualification; server minimum TBD |
| Pulsar | Server/protocol minimums TBD |
| AMQP 0.9.1 | Separate wire implementation; TCP/TLS |
| AMQP 1.0 | Separate wire implementation; TCP/TLS |
| MQTT | MQTT 5.0; TCP/TLS |
| AWS SQS | API version TBD; HTTPS |
| AWS SNS | API version TBD; HTTPS |
| Azure Service Bus | AMQP 1.0/HTTPS; API version TBD |
| Google Pub/Sub | gRPC/HTTPS; API version TBD |
| Protobuf | Library/version TBD |
| Avro | Library/version TBD |
| BSON | Codec/version TBD |
| MessagePack | Codec/version TBD |
| Schema Registry integration | Provider/API version TBD; HTTP |
| S3-compatible object storage | S3 API; signing and per-provider qualification |
| Azure Blob Storage | API version TBD; HTTPS |
| Google Cloud Storage | API version TBD; HTTPS |
| Blocking-work executor | Bounded worker pool; Task/executor integration |
| Asynchronous file I/O | Platform file backends |
| Consistent metrics and tracing | Existing runtime/protocol hooks |
| Linux CI | Linux build requirements above |
| Protocol fuzzing and interoperability testing | Protocol-specific peers and harnesses |
| API stability and versioning policy | Policy TBD |
| Project license | License TBD |
