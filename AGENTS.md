# Weave development rules

- Windows IOCP first; Linux io_uring later. No epoll or macOS backend.
- C++23, CMake, exceptions disabled. Public asynchronous type is Async<T>.
- Use explicit Result<T> errors. Never introduce throwing operational APIs.
- Performance comes before API convenience, with correctness as a constraint.
- Treat "data driven" as both evidence-driven engineering and data-oriented
  storage: explicit ownership, compact hot state, batched work, no speculative
  object hierarchies or per-operation shared ownership.
- Benchmark against the pinned Asio baseline using the same workload and build.
  Report regressions and uncertainty. Do not claim wins from noisy single runs.
- Keep read/write buffers and OVERLAPPED records alive through completion,
  including cancellation. Never silently destroy active coroutine frames.
- Add tests for immediate failure, pending completion, EOF, partial I/O, and
  cancellation when changing those paths. Run Debug and Release CTest.
- Keep API documentation honest about unsupported features and fatal contracts.
- Do not implement TLS, HTTP parsing, or cryptography from scratch.
- Do not commit build products. Preserve deliberate benchmark evidence only.
