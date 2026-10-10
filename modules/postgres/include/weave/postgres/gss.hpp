#pragma once

#include <weave/task.hpp>
#include <memory>
#include <string>
#include <cstddef>

namespace weave::pg {

namespace detail {

class GssPool;
class GssSession;

} // namespace detail

enum class GssEncryption {
  disable,
  prefer,
  require
};

struct GssContextOptions {
  std::size_t workers = 2;
  std::size_t capacity = 64;
  // Linux: empty selects the system default cache at creation, not on a worker.
  std::string credential_cache;
};

class GssContext {
  friend class detail::GssSession;
  std::shared_ptr<detail::GssPool> pool_;

  explicit GssContext(std::shared_ptr<detail::GssPool> pool) noexcept : pool_(std::move(pool))
  {
  }

public:
  // Explicit local setup: creates bounded provider workers and captures the credential source.
  static Result<GssContext> create(GssContextOptions options = {});
};

} // namespace weave::pg
