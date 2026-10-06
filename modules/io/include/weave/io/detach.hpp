#pragma once

#include <weave/io/context.hpp>
#include <weave/io/detail/scheduled_spawn.hpp>

namespace weave {

// Inherit the executing scope's ownership and scheduling policy. Never joins the parent.
template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
void detach(F &&factory, SpawnOptions options, H on_error = {})
{
  detail::require(detail::current_context != nullptr);
  const auto *scope = detail::current_submission;
  if (!scope) {
    detail::current_context->detach(std::forward<F>(factory), options, std::move(on_error));
    return;
  }

  using Function = std::decay_t<F>;
  auto *task = new (std::nothrow) detail::SpawnTask<Function, detail::ScheduledSpawn, H>(
    Function(std::forward<F>(factory)),
    detail::SpawnMode::detached,
    std::move(on_error),
    options);
  detail::require(task != nullptr);

  auto accepted = scope->submit(scope->state, scope->worker, *task);
  if (!accepted) {
    task->report_error(accepted.error());
    delete task;
  }
}

template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
void detach(F &&factory, H on_error = {})
{
  weave::detach(std::forward<F>(factory), SpawnOptions{}, std::move(on_error));
}

template <class T, detail::ErrorObserver H = detail::IgnoreError>
void detach(Task<T> operation, SpawnOptions options, H on_error = {})
{
  weave::detach(detail::TaskSubmission<T>{std::move(operation)}, options, std::move(on_error));
}

template <class T, detail::ErrorObserver H = detail::IgnoreError>
void detach(Task<T> operation, H on_error = {})
{
  weave::detach(detail::TaskSubmission<T>{std::move(operation)}, std::move(on_error));
}

} // namespace weave
