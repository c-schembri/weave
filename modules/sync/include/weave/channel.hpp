#pragma once

#include <weave/sync/detail/wait.hpp>
#include <vector>

namespace weave {

template <class T>
  requires std::is_nothrow_move_constructible_v<T> && std::is_nothrow_destructible_v<T>
class Channel {
  struct Sender : detail::SyncWait {
    Channel &channel;
    T value;

    Sender(Channel &owner, T item) noexcept : SyncWait(owner.domain_), channel(owner), value(std::move(item))
    {
    }

    bool await_ready() const noexcept
    {
      return false;
    }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> continuation)
    {
      this->prepare(continuation);
      std::lock_guard lock(this->domain.mutex);

      if (this->error) {
        this->finish(this->error);
        return false;
      }

      if (this->domain.closed) {
        this->finish(make_error_code(SyncError::closed));
        return false;
      }

      if (channel.deliver(value)) {
        this->finish();
        return false;
      }

      channel.senders_.push(*this);
      return true;
    }

    Result<void> await_resume() noexcept
    {
      this->disarm();
      std::lock_guard lock(this->domain.mutex);

      detail::require(this->phase == detail::SyncWait::Phase::done);
      if (this->error)
        return std::unexpected(this->error);

      return {};
    }
  };

  struct Receiver : detail::SyncWait {
    Channel &channel;
    std::optional<T> value;

    explicit Receiver(Channel &owner) noexcept : SyncWait(owner.domain_), channel(owner)
    {
    }

    bool await_ready() const noexcept
    {
      return false;
    }

    template <class P>
    bool await_suspend(std::coroutine_handle<P> continuation)
    {
      this->prepare(continuation);
      std::lock_guard lock(this->domain.mutex);

      if (this->error) {
        this->finish(this->error);
        return false;
      }

      if (channel.size_) {
        value.emplace(channel.take());
        this->finish();
        return false;
      }

      if (this->domain.closed) {
        this->finish();
        return false;
      }

      channel.receivers_.push(*this);
      return true;
    }

    Result<std::optional<T>> await_resume() noexcept
    {
      this->disarm();
      std::lock_guard lock(this->domain.mutex);

      detail::require(this->phase == detail::SyncWait::Phase::done);
      if (this->error)
        return std::unexpected(this->error);

      return std::move(value);
    }
  };

public:
  explicit Channel(std::size_t capacity) : buffer_(capacity)
  {
    detail::require(capacity > 0);
  }

  Channel(const Channel &) = delete;

  ~Channel()
  {
    detail::require(senders_.empty() && receivers_.empty());
  }

  Task<void> send(T value)
  {
    auto result = co_await Sender{*this, std::move(value)};
    if (!result)
      co_await fail(result.error());
  }

  Task<std::optional<T>> receive()
  {
    auto result = co_await Receiver{*this};
    if (!result)
      co_await fail(result.error());

    co_return std::move(*result);
  }

  // A failed try_send leaves the caller's value unchanged.
  Result<void> try_send(T &value) noexcept
  {
    std::lock_guard lock(domain_.mutex);

    if (domain_.closed)
      return std::unexpected(make_error_code(SyncError::closed));
    if (!deliver(value))
      return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));

    return {};
  }

  Result<std::optional<T>> try_receive() noexcept
  {
    std::lock_guard lock(domain_.mutex);

    if (size_)
      return std::optional<T>{take()};
    if (domain_.closed)
      return std::optional<T>{};

    return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
  }

  void close() noexcept
  {
    std::lock_guard lock(domain_.mutex);
    domain_.closed = true;

    while (!senders_.empty())
      senders_.first->finish(make_error_code(SyncError::closed));

    while (!receivers_.empty())
      receivers_.first->finish();
  }

private:
  detail::WaitDomain domain_;
  detail::WaitQueue senders_;
  detail::WaitQueue receivers_;

  std::vector<std::optional<T>> buffer_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;

  bool deliver(T &value) noexcept
  {
    if (!receivers_.empty()) {
      auto &receiver = *static_cast<Receiver *>(receivers_.first);
      receiver.value.emplace(std::move(value));
      receiver.finish();
      return true;
    }

    if (size_ == buffer_.size())
      return false;

    buffer_[(head_ + size_) % buffer_.size()].emplace(std::move(value));
    ++size_;

    return true;
  }

  T take() noexcept
  {
    auto &slot = buffer_[head_];
    T value(std::move(*slot));
    slot.reset();
    head_ = (head_ + 1) % buffer_.size();
    --size_;

    if (!senders_.empty()) {
      auto &sender = *static_cast<Sender *>(senders_.first);
      detail::require(deliver(sender.value));
      sender.finish();
    }

    return value;
  }
};

} // namespace weave
