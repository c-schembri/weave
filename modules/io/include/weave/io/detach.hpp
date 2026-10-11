#pragma once

#include <weave/io/context.hpp>
#include <weave/io/detail/scheduled_spawn.hpp>

namespace weave {

/// Submit owned independent work using the executing scope's Context/Runtime scheduling policy.
/// Requires active Weave execution; outside it use Context/Runtime members. Does not join its parent.
/// Factory/observer objects are owned, captured references are not; options.cancel controls cancellation.
/// Rejection is observed synchronously on the submitter, execution failure after cleanup on an executor;
/// an omitted noexcept observer discards errors.
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

/// Detached factory submission using the active execution scope, without implicit parent cancellation.
/// Owns the factory through completion; captured references must outlive the independent task.
template <detail::SpawnFactory F, detail::ErrorObserver H = detail::IgnoreError>
void detach(F &&factory, H on_error = {})
{
  weave::detach(std::forward<F>(factory), SpawnOptions{}, std::move(on_error));
}

/// Transfer an unstarted Task to active execution with explicit cancellation; no join handle is returned.
/// The Task owns its frame, not referenced arguments or an originating coroutine-lambda closure.
template <class T, detail::ErrorObserver H = detail::IgnoreError>
void detach(Task<T> operation, SpawnOptions options, H on_error = {})
{
  weave::detach(detail::TaskSubmission<T>{std::move(operation)}, options, std::move(on_error));
}

/// Transfer an unstarted Task to active execution without implicit parent cancellation or joining.
/// Omitted error observers discard failures; use explicit Context/Runtime submission outside active execution.
template <class T, detail::ErrorObserver H = detail::IgnoreError>
void detach(Task<T> operation, H on_error = {})
{
  weave::detach(detail::TaskSubmission<T>{std::move(operation)}, std::move(on_error));
}

} // namespace weave
