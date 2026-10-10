#pragma once

#include "gss.hpp"
#include <weave/postgres/gss.hpp>
#include <weave/io/detail/execution.hpp>
#include <weave/semaphore.hpp>
#include <condition_variable>
#include <mutex>

namespace weave::pg::detail {

struct GssWork {
  void (*invoke)(GssWork &) noexcept = nullptr;
  GssWork *next = nullptr;
};

struct OwnedGss : GssWork {
  Gss engine;
  GssPool *owner = nullptr;
};

class GssPool {
  struct Platform;
  std::unique_ptr<Platform> platform_;
  GssContextOptions options_;
  std::mutex mutex_;
  std::condition_variable ready_;
  GssWork *first_ = nullptr;
  GssWork *last_ = nullptr;
  std::size_t sessions_ = 0;
  std::size_t queued_ = 0;
  std::size_t retired_ = 0;
  std::size_t started_ = 0;
  bool stopping_ = false;
  std::error_code startup_error_;

  void run() noexcept;
  void stop() noexcept;
  void drained() noexcept;
  void enqueue(GssWork &work) noexcept;

public:
  explicit GssPool(GssContextOptions options);
  ~GssPool();
  GssPool(const GssPool &) = delete;

  Result<void> start();
  Result<void> wait_started();
  void worker(std::error_code startup_error) noexcept;
  Result<void> acquire();
  void release() noexcept;
  void submit(GssWork &work) noexcept;
  void retire(std::unique_ptr<OwnedGss> engine) noexcept;
  void release_retired() noexcept;

  const std::string &credential_cache() const noexcept
  {
    return options_.credential_cache;
  }
};

class GssSession {
  std::shared_ptr<GssPool> pool_;
  std::unique_ptr<OwnedGss> engine_;
  Semaphore gate_{1};
  mutable std::mutex metadata_;
  std::string diagnostic_;
  std::size_t plaintext_limit_ = 0;
  bool complete_ = false;
  bool pending_ = false;

  Task<void> close_impl();
  void capture_metadata();

public:
  GssSession() = default;
  ~GssSession();
  GssSession(const GssSession &) = delete;

  Task<GssToken> start(const GssContext &context, std::string host, Authentication method, GssOptions options);
  Task<GssToken> next(std::span<const std::byte> input);
  Task<SecretStorage<std::byte>> wrap(std::span<const std::byte> input);
  Task<SecretStorage<std::byte>> unwrap(std::span<const std::byte> input);
  Task<void> close();

  std::size_t plaintext_limit() const noexcept
  {
    std::lock_guard lock(metadata_);
    return plaintext_limit_;
  }

  bool complete() const noexcept
  {
    std::lock_guard lock(metadata_);
    return complete_;
  }

  std::string diagnostic() const
  {
    std::lock_guard lock(metadata_);
    return diagnostic_;
  }
};

} // namespace weave::pg::detail
