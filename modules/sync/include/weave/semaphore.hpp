#pragma once

#include <weave/sync/detail/wait.hpp>

namespace weave {

class Semaphore {
public:
  class [[nodiscard]] Permit {
    friend class Semaphore;
    Semaphore *owner_ = nullptr;

    explicit Permit(Semaphore &owner) noexcept : owner_(&owner)
    {
    }

  public:
    Permit(Permit &&other) noexcept : owner_(std::exchange(other.owner_, nullptr))
    {
    }

    Permit &operator=(Permit &&other) noexcept;
    Permit(const Permit &) = delete;
    ~Permit();

    void release() noexcept;
  };

  explicit Semaphore(std::size_t permits) noexcept : available_(permits)
  {
    detail::require(permits > 0);
  }

  Semaphore(const Semaphore &) = delete;
  ~Semaphore();

  Task<Permit> acquire();
  Result<Permit> try_acquire() noexcept;
  void close() noexcept;

private:
  struct Acquire;
  detail::WaitDomain domain_;
  detail::WaitQueue waiters_;
  std::size_t available_;
  std::size_t held_ = 0;

  void release() noexcept;
};

} // namespace weave
