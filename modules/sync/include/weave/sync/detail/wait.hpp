#pragma once

#include <weave/task.hpp>
#include <weave/io/detail/execution.hpp>
#include <mutex>

namespace weave {

enum class SyncError {
  closed = 1
};

Error make_error_code(SyncError error) noexcept;

namespace detail {

struct WaitQueue;

struct WaitDomain {
  std::mutex mutex;
  bool closed = false;
};

struct SyncWait;

struct CancelWait {
  SyncWait *wait;

  void operator()() const noexcept;
};

struct SyncWait {
  enum class Phase {
    initial,
    queued,
    done
  };

  WaitDomain &domain;
  WaitQueue *queue = nullptr;
  SyncWait *previous = nullptr;
  SyncWait *next = nullptr;

  Posted event;
  Error error;
  Phase phase = Phase::initial;

  std::optional<std::stop_callback<CancelWait>> cancellation;
  std::optional<std::stop_callback<CancelWait>> shutdown;

  explicit SyncWait(WaitDomain &owner) noexcept : domain(owner)
  {
  }

  SyncWait(const SyncWait &) = delete;
  ~SyncWait();

  template <class P>
  void prepare(std::coroutine_handle<P> continuation)
  {
    require(current_context != nullptr);

    event.target = current_context;
    event.executor = current_executor;
    event.state = continuation.address();
    event.invoke = [](void *address) noexcept {
      std::coroutine_handle<>::from_address(address).resume();
    };

    // Register before publishing the waiter. Immediate cancellation only marks its initial state.
    cancellation.emplace(continuation.promise().cancellation.native_token(), CancelWait{this});
    shutdown.emplace(context_cancellation(*event.target).native_token(), CancelWait{this});
  }

  void finish(Error result = {}) noexcept;
  void disarm() noexcept;
};

struct WaitQueue {
  SyncWait *first = nullptr;
  SyncWait *last = nullptr;

  bool empty() const noexcept
  {
    return first == nullptr;
  }

  void push(SyncWait &wait) noexcept;
  void remove(SyncWait &wait) noexcept;
};

} // namespace detail
} // namespace weave

namespace std {

template <>
struct is_error_code_enum<weave::SyncError> : true_type {};

} // namespace std
