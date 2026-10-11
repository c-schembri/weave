#pragma once

#include <cstddef>

namespace bench {

struct Allocations {
  std::size_t calls = 0;
  std::size_t bytes = 0;
};

// Counts C++ allocation requests on the driving thread, not C/DLL allocators or RSS.
class AllocationSample {
public:
  explicit AllocationSample(bool enabled) noexcept;
  ~AllocationSample();
  Allocations finish() noexcept;
};

} // namespace bench
