#pragma once

#include <weave/sync/detail/wait.hpp>

namespace weave {

/// Thread-safe bounded permit source. Must outlive all waiters and acquired Permits.
/// close() wakes waiters but does not join them; destruction requires fully drained use.
class Semaphore {
public:
  /// Move-only lease returning its permit on destruction; borrows the Semaphore.
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

    /// Return the permit immediately; repeated release calls do nothing.
    void release() noexcept;
  };

  /// Create a permit source; permits must be positive (a contract requirement).
  explicit Semaphore(std::size_t permits) noexcept : available_(permits)
  {
    detail::require(permits > 0);
  }

  Semaphore(const Semaphore &) = delete;
  ~Semaphore();

  /// Lazily wait for a permit on an active Context; cancellation fails with operation_canceled.
  /// A closed source fails with SyncError::closed. The returned lease borrows this object.
  Task<Permit> acquire();
  /// Acquire synchronously or return resource_unavailable_try_again; closed is SyncError::closed.
  Result<Permit> try_acquire() noexcept;
  /// Thread-safely reject new acquisitions and wake waiters; does not reclaim outstanding permits.
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
