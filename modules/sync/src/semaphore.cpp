#include <weave/semaphore.hpp>

namespace weave {

struct Semaphore::Acquire : detail::SyncWait {
  Semaphore &semaphore;

  explicit Acquire(Semaphore &owner) noexcept : SyncWait(owner.domain_), semaphore(owner)
  {
  }

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> continuation)
  {
    prepare(continuation);
    std::lock_guard lock(domain.mutex);

    if (error) {
      finish(error);
      return false;
    }

    if (domain.closed) {
      finish(make_error_code(SyncError::closed));
      return false;
    }

    if (semaphore.available_) {
      --semaphore.available_;
      ++semaphore.held_;
      finish();
      return false;
    }

    semaphore.waiters_.push(*this);
    return true;
  }

  Result<Permit> await_resume() noexcept
  {
    disarm();
    std::lock_guard lock(domain.mutex);

    detail::require(phase == Phase::done);
    if (error)
      return std::unexpected(error);

    return Permit{semaphore};
  }
};

Semaphore::~Semaphore()
{
  detail::require(waiters_.empty() && held_ == 0);
}

Task<Semaphore::Permit> Semaphore::acquire()
{
  auto result = co_await Acquire{*this};
  if (!result)
    co_await fail(result.error());

  co_return std::move(*result);
}

Result<Semaphore::Permit> Semaphore::try_acquire() noexcept
{
  std::lock_guard lock(domain_.mutex);

  if (domain_.closed)
    return std::unexpected(make_error_code(SyncError::closed));
  if (!available_)
    return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));

  --available_;
  ++held_;

  return Permit{*this};
}

void Semaphore::release() noexcept
{
  std::lock_guard lock(domain_.mutex);
  detail::require(held_ > 0);

  if (!waiters_.empty())
    waiters_.first->finish();
  else {
    --held_;
    ++available_;
  }
}

void Semaphore::close() noexcept
{
  std::lock_guard lock(domain_.mutex);
  domain_.closed = true;

  while (!waiters_.empty())
    waiters_.first->finish(make_error_code(SyncError::closed));
}

Semaphore::Permit::~Permit()
{
  release();
}

void Semaphore::Permit::release() noexcept
{
  if (auto *owner = std::exchange(owner_, nullptr))
    owner->release();
}

Semaphore::Permit &Semaphore::Permit::operator=(Permit &&other) noexcept
{
  if (this != &other) {
    release();
    owner_ = std::exchange(other.owner_, nullptr);
  }

  return *this;
}

} // namespace weave
