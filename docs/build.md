# Build And Installation

## Requirements

| Component | Requirements |
| --- | --- |
| All | C++23, CMake 3.25+, Git; exceptions disabled |
| Windows networking | Windows x64, Visual Studio 2022 C++ workload |
| Linux networking | GCC 14+, glibc, liburing 2.3+, pkg-config |
| TLS | Separately installed, security-patched OpenSSL 3.5+ |
| PostgreSQL | TLS dependencies, ICU 70+, a C compiler for private vendored parsers |
| MySQL | TLS dependencies; no native MySQL client SDK |
| Tests / automation | Python 3.11+; standard library only |
| Optional Tokio comparison | Rust toolchain |

Linux needs io_uring enabled by host policy and IORING_FEAT_EXT_ARG/NODROP
(Linux 5.11+). Context creation reports native setup failure if ring creation
is denied; there is no epoll fallback. Linux GSS support defaults on and needs
MIT Kerberos development packages (`krb5-gssapi` and `krb5` pkg-config modules).
Set `WEAVE_POSTGRES_GSSAPI=OFF` to omit it. Windows uses system SSPI.
LDAP configuration lookup is a separate `WEAVE_POSTGRES_LDAP=ON` opt-in.

## Include Only What You Need

```cmake
set(WEAVE_MODULES "tcp;runtime" CACHE STRING "" FORCE)
add_subdirectory(external/weave)

target_link_libraries(my_app PRIVATE weave::tcp weave::runtime)
```

Required dependencies are selected automatically. Core, Database and JSON are header-only;
other modules are static libraries. Database depends only on Core. Its private async
pool support uses IO/Sync without becoming an installed component. TCP does not
require Runtime, and Runtime does not require TCP. TLS adds TCP/Sync/OpenSSL;
PostgreSQL adds Database, private JSON parsing, TLS, private local transport, OpenSSL Crypto and ICU.
MySQL adds Database and TLS, with private OpenSSL Crypto authentication; it does not depend on PostgreSQL or Runtime.
JSON uses pinned vendored Glaze headers; no consumer fetch or separately installed package is required.
There is no all-features umbrella target or shared-library ABI/export support.

| Component | Header | CMake target |
| --- | --- | --- |
| Tasks, results, stream helpers | `<weave/core.hpp>`, `<weave/stream.hpp>` | `weave::core` |
| JSON struct conversion | `<weave/json.hpp>` | `weave::json` |
| Database storage, mapping and schema descriptions | `<weave/database.hpp>` | `weave::database` |
| Context and timers | `<weave/io.hpp>` | `weave::io` |
| Multicore runtime | `<weave/runtime.hpp>` | `weave::runtime` |
| TCP | `<weave/tcp.hpp>` | `weave::tcp` |
| Local sockets | `<weave/local.hpp>` | `weave::local` |
| Channels/semaphores | `<weave/sync.hpp>` | `weave::sync` |
| TLS | `<weave/tls.hpp>` | `weave::tls` |
| PostgreSQL | `<weave/postgres.hpp>` | `weave::postgres` |
| MySQL | `<weave/mysql.hpp>` | `weave::mysql` |

Library-only builds do not fetch test/comparison dependencies. Tests, examples
and benchmarks are independent opt-ins, off by default:
`WEAVE_BUILD_TESTS`, `WEAVE_BUILD_EXAMPLES`, `WEAVE_BUILD_BENCHMARKS`.
PostgreSQL comparisons additionally require `WEAVE_POSTGRES_BENCHMARKS=ON`.
Private vendored parser headers/targets are not exported; their licenses are installed.

## Install And Consume

```sh
cmake -S . -B build/tcp -DWEAVE_MODULES=tcp
cmake --build build/tcp --config Release
cmake --install build/tcp --config Release --prefix C:/Libraries/weave
```

Set the consumer's `CMAKE_PREFIX_PATH` to that installation:

```cmake
find_package(weave CONFIG REQUIRED COMPONENTS tcp)
target_link_libraries(my_app PRIVATE weave::tcp)
```

Components are `core`, `json`, `database`, `io`, `runtime`, `tcp`, `local`, `sync`, `tls`,
`postgres` and `mysql`; each exports `weave::<component>`. Weave's public headers are self-contained
and have no native OS header dependency.

Consumers must deploy shared OpenSSL/ICU runtime libraries. Development targets
copy detected Windows DLLs; override `WEAVE_TLS_RUNTIME_DIR` /
`WEAVE_POSTGRES_RUNTIME_DIR` when needed. These copy rules are not an installed
application deployment mechanism.

## Windows Development

```sh
cmake --preset windows-postgres -DOPENSSL_ROOT_DIR=C:/Libraries/openssl -DICU_ROOT=C:/Libraries/icu
cmake --build --preset postgres-debug --parallel 4
ctest --preset postgres-debug --label-exclude benchmark --no-tests=error
```

| Configure preset | Build/test preset | Contents |
| --- | --- | --- |
| `windows-ci` | `ci-debug`, `ci-release` | Base networking, tests and examples; no benchmarks |
| `windows-tls` | `tls-debug`, `tls-release` | Adds TLS |
| `windows-postgres` | `postgres-debug`, `postgres-release` | Adds PostgreSQL and MySQL |
| `asan-postgres` | `asan-postgres` | Release AddressSanitizer, including PostgreSQL and MySQL |
| `windows-runtime-bench` | `runtime-bench` (build only) | Local runtime comparisons |
| `windows` | `debug`, `release` | Full developer suite and comparison dependencies |

MSVC ASan requires the Visual Studio AddressSanitizer component. CTest adds its
runtime path. Hosted Windows CI pins dependency versions and runs correctness
only, not measurements.

## Linux And WSL

Install a sufficiently recent compiler and distribution packages, for example:

```sh
sudo apt-get install g++ cmake ninja-build liburing-dev libssl-dev libicu-dev libkrb5-dev pkg-config git python3
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel 4
ctest --preset linux-debug --no-tests=error
```

Distribution OpenSSL must meet the version requirement. Use `linux-release`
or `linux-asan` for other configurations. These presets include TLS, not
database clients; add `-DWEAVE_MODULES="mysql;runtime"` for MySQL or
`-DWEAVE_MODULES="postgres;mysql;runtime"` for both drivers and their dependencies.
No macOS backend is implemented.

WSL uses the real Linux backend. Prefer a Linux-filesystem build directory when
source is under `/mnt/c`:

```sh
cmake -S /mnt/c/Projects/weave -B ~/.cache/weave -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWEAVE_MODULES="postgres;runtime" -DWEAVE_BUILD_TESTS=ON
cmake --build ~/.cache/weave --parallel 4
ctest --test-dir ~/.cache/weave --output-on-failure --no-tests=error
```

[Correctness and optional deployment gates](../development/testing/README.md) /
[Local benchmark instructions](../development/benchmarks/README.md).
