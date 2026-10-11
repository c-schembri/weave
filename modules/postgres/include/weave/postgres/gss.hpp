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

/// GSS transport encryption policy, independent of whether GSS authenticates the session.
enum class GssEncryption {
  disable,
  prefer,
  require
};

/// Owned bounded provider-worker configuration and explicit credential source.
struct GssContextOptions {
  std::size_t workers = 2;
  std::size_t capacity = 64;
  // Linux: empty selects the system default cache at creation, not on a worker.
  std::string credential_cache;
};

/// Shared GSS provider/credential-source ownership; copies retain the same bounded provider pool.
class GssContext {
  friend class detail::GssSession;
  std::shared_ptr<detail::GssPool> pool_;

  explicit GssContext(std::shared_ptr<detail::GssPool> pool) noexcept : pool_(std::move(pool))
  {
  }

public:
  /// Synchronous local setup: copy/move options, create provider workers and capture credential source now.
  /// On Linux an empty cache selects the system default at creation, not later on a worker.
  static Result<GssContext> create(GssContextOptions options = {});
};

} // namespace weave::pg
