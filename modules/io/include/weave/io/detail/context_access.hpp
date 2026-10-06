#pragma once

#include <weave/io/context.hpp>

namespace weave::detail {

struct IoDomain;

struct ContextAccess {
  static Result<void> submit(Context &context, SpawnBase &task)
  {
    return context.submit(task);
  }

  static Result<std::shared_ptr<IoDomain>> create_domain(
    std::size_t concurrency,
    TaskObserver collectors = {}) noexcept;
  static Result<Context> create(ContextOptions options, const std::shared_ptr<IoDomain> &domain) noexcept;

  static void post(Context &context, Posted &message) noexcept
  {
    context.post(message);
  }

  static void enter(Context &context, void *scheduler_group = nullptr) noexcept
  {
    context.enter(scheduler_group);
  }

  static void leave(Context &context) noexcept
  {
    context.leave();
  }

  static void poll(Context &context, bool wait = true)
  {
    context.poll(wait);
  }

  static void cancel(Context &context) noexcept
  {
    context.request_stop();
    context.cancel_pending();
  }

  static void close_submissions(Context &context) noexcept
  {
    context.close_submissions();
  }

  static void observe(Context &context, TaskObserver observer) noexcept
  {
    context.observe(observer);
  }

  static void wake(Context &context) noexcept;
};

} // namespace weave::detail
