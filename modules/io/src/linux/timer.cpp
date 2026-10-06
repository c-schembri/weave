#include "uring.hpp"
#include <weave/timer.hpp>

namespace weave {

namespace {

struct TimerCancel {
  detail::TimerRecord *timer;

  void operator()() const noexcept
  {
    detail::IoAccess::cancel_timer(*timer);
  }
};

struct TimerAwaiter {
  detail::TimerRecord timer;
  std::optional<std::stop_callback<TimerCancel>> cancellation;
  std::optional<std::stop_callback<TimerCancel>> shutdown;

  explicit TimerAwaiter(std::chrono::steady_clock::time_point deadline) : timer{.deadline = deadline, .error = {}}
  {
  }

  bool await_ready() const noexcept
  {
    return false;
  }

  template <class P>
  bool await_suspend(std::coroutine_handle<P> continuation)
  {
    detail::require(detail::current_context != nullptr);
    timer.context = detail::current_context;
    auto token = continuation.promise().cancellation;
    auto stopping = detail::context_cancellation(*timer.context);
    if (token.stop_requested() || stopping.stop_requested()) {
      timer.error = std::make_error_code(std::errc::operation_canceled);
      return false;
    }
    if (timer.deadline <= std::chrono::steady_clock::now())
      return false;

    timer.event.executor = detail::current_executor;
    timer.event.state = continuation.address();
    timer.event.invoke = [](void *address) noexcept {
      std::coroutine_handle<>::from_address(address).resume();
    };
    detail::IoAccess::start_timer(timer);
    cancellation.emplace(token.native_token(), TimerCancel{&timer});
    shutdown.emplace(stopping.native_token(), TimerCancel{&timer});
    return true;
  }

  Result<void> await_resume() noexcept
  {
    cancellation.reset();
    shutdown.reset();
    if (timer.error)
      return std::unexpected(timer.error);
    return {};
  }
};

} // namespace

Task<void> sleep_until(std::chrono::steady_clock::time_point deadline)
{
  TimerAwaiter timer(deadline);
  auto result = co_await timer;
  if (!result)
    co_await fail(result.error());
}

} // namespace weave
