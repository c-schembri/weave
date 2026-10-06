# Build And Installation

Windows x64: Visual Studio 2022 with the C++ workload, CMake 3.25+, and Git.
Core is portable C++23; IO, TCP, and Runtime currently require Windows. Public
targets supply their C++23 requirement. Python 3.11+ is needed only for tests
and developer automation, without pip packages. Rust is needed only for the
optional Tokio comparison. Library-only builds download no external dependencies.

## Vendored

```cmake
set(WEAVE_MODULES tcp CACHE STRING "" FORCE)
add_subdirectory(external/weave)
target_link_libraries(my_app PRIVATE weave::tcp)
```

Select `"tcp;runtime"` and link `weave::runtime` too when using worker threads.
Required components are selected automatically: TCP and Runtime depend on IO,
IO depends on core. TCP and Runtime do not depend on each other.

## Installed

```sh
cmake -S . -B build/tcp -DWEAVE_MODULES=tcp
cmake --build build/tcp --config Release
cmake --install build/tcp --config Release --prefix C:/Libraries/weave
```

Set `CMAKE_PREFIX_PATH` to that installation in the consuming project:

```cmake
find_package(weave CONFIG REQUIRED COMPONENTS tcp)
target_link_libraries(my_app PRIVATE weave::tcp)
```

Only core is header-only; the other components are static libraries. Shared-library
ABI/export support is not implemented. Public headers are self-contained and have
no native Windows header dependency.

## Development Presets

| Configure | Build/test | Contents |
| --- | --- | --- |
| `windows-ci` | `ci-debug`, `ci-release` | Correctness tests and examples; no benchmarks |
| `asan` | `asan` | Release AddressSanitizer tests/examples; no benchmarks |
| `windows-runtime-bench` | `runtime-bench` (build only) | Only native Weave/Asio runtime servers and common client |
| `windows` | `debug`, `release` | Full tests, examples, comparisons and benchmark smokes |

Tests, examples and benchmarks are separate opt-ins: `WEAVE_BUILD_TESTS`,
`WEAVE_BUILD_EXAMPLES`, `WEAVE_BUILD_BENCHMARKS`. All default off for consumers.
Benchmark builds select `WEAVE_BENCHMARK_SUITE=full` or `runtime`; correctness presets
never execute native benchmarks. Dependencies are pinned in CMake/Cargo.lock.

```sh
cmake --preset windows-ci
cmake --build --preset ci-debug --parallel 4
ctest --preset ci-debug
```

For ASan, install the MSVC AddressSanitizer component. CTest supplies its runtime
DLL path. Packaging tests cover isolated modules, relocated consumers, and every
installed header. See [examples](../modules/tcp/examples/echo/README.md),
[local runtime scaling](runtime-scaling.md), and [manual benchmarks](benchmarks.md).
