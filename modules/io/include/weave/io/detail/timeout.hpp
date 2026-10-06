#pragma once

#include <weave/timer.hpp>

namespace weave::detail {

enum class TimeoutWinner {
  none,
  operation,
  deadline,
  cancelled
};

template <class T>
struct TimeoutState {
  CancelSource operation_stop;
  CancelSource timer_stop;
  std::optional<Result<T>> result;
  TimeoutWinner winner = TimeoutWinner::none;
};

template <class T>
Task<void> timeout_operation(TimeoutState<T> &state, Task<T> operation)
{
  TaskAccess::bind(operation, state.operation_stop.token());
  state.result.emplace(co_await as_result(std::move(operation)));
  if (state.winner == TimeoutWinner::none) {
    state.winner = TimeoutWinner::operation;
    state.timer_stop.cancel();
  }
}

template <class T>
Task<void> timeout_timer(TimeoutState<T> &state, std::chrono::steady_clock::time_point deadline)
{
  auto timer = sleep_until(deadline);
  TaskAccess::bind(timer, state.timer_stop.token());
  auto result = co_await as_result(std::move(timer));
  if (state.winner == TimeoutWinner::none) {
    state.winner = result ? TimeoutWinner::deadline : TimeoutWinner::cancelled;
    state.operation_stop.cancel();
  }
}

} // namespace weave::detail

namespace weave {

template <class T>
Task<T> timeout_at(std::chrono::steady_clock::time_point deadline, Task<T> operation)
{
  detail::TimeoutState<T> state;
  auto parent = co_await detail::GetCancellation{};
  detail::CancelLink operation_link(parent.native_token(), detail::ForwardCancel{state.operation_stop});
  detail::CancelLink timer_link(parent.native_token(), detail::ForwardCancel{state.timer_stop});

  auto work = detail::timeout_operation(state, std::move(operation));
  auto timer = detail::timeout_timer(state, deadline);
  detail::TaskAccess::bind(work, {});
  detail::TaskAccess::bind(timer, {});
  auto cleanup = when_all(std::move(work), std::move(timer));
  detail::TaskAccess::bind(cleanup, {});
  co_await std::move(cleanup);

  // Never return while a native operation can still access its frame or borrowed buffers.
  if (state.winner == detail::TimeoutWinner::deadline)
    co_await fail(std::errc::timed_out);
  if (state.winner == detail::TimeoutWinner::cancelled)
    co_await fail(std::errc::operation_canceled);
  detail::require(state.result.has_value());
  if (!*state.result)
    co_await fail(state.result->error());
  if constexpr (!std::is_void_v<T>)
    co_return std::move(**state.result);
}

template <class Rep, class Period, class T>
Task<T> timeout(std::chrono::duration<Rep, Period> duration, Task<T> operation)
{
  auto deadline = detail::deadline_after(duration);
  if (!deadline)
    co_await fail(deadline.error());
  if constexpr (std::is_void_v<T>)
    co_await timeout_at(*deadline, std::move(operation));
  else
    co_return co_await timeout_at(*deadline, std::move(operation));
}

} // namespace weave
