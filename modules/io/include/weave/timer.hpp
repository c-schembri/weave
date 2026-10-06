#pragma once

#include <weave/task.hpp>
#include <chrono>
#include <cmath>

namespace weave {

[[nodiscard]] Task<void> sleep_until(std::chrono::steady_clock::time_point deadline);

namespace detail {

template <class Rep, class Period>
Result<std::chrono::steady_clock::time_point> deadline_after(std::chrono::duration<Rep, Period> duration) noexcept
{
  using Clock = std::chrono::steady_clock;
  using Ticks = std::chrono::duration<long double, Clock::period>;
  const auto ticks = std::ceil(Ticks(duration).count());
  if (std::isnan(ticks))
    return std::unexpected(std::make_error_code(std::errc::invalid_argument));
  const auto now = Clock::now();
  if (ticks <= 0)
    return now;
  if (ticks >= static_cast<long double>(Clock::duration::max().count()))
    return Clock::time_point::max();
  const auto delay = Clock::duration{static_cast<Clock::rep>(ticks)};
  if (now > Clock::time_point::max() - delay)
    return Clock::time_point::max();
  return now + delay;
}

} // namespace detail

// Relative time starts when this lazy task executes, not when it is constructed.
template <class Rep, class Period>
Task<void> sleep_for(std::chrono::duration<Rep, Period> duration)
{
  auto deadline = detail::deadline_after(duration);
  if (!deadline)
    co_await fail(deadline.error());
  co_await sleep_until(*deadline);
}

} // namespace weave

#include <weave/io/detail/timeout.hpp>
