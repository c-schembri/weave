# Development Infrastructure

| Component | Owns |
| --- | --- |
| [build/](build/) | Shared CMake target policies |
| [packaging/](packaging/) | Package configuration, installation and consumer tests |
| [testing/](testing/README.md) | Cross-module correctness tests and shared fixtures |
| [benchmarks/](benchmarks/README.md) | Shared comparison harnesses, orchestration and tooling tests |

Feature implementations, guides, tests, examples and feature-specific tools belong
under `modules/<feature>/`. Development infrastructure is not a public library component.
