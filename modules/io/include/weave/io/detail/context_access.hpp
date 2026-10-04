#pragma once

#include <weave/io/context.hpp>

namespace weave::detail {

struct ContextAccess {
  static void post(Context &context, Posted &message) noexcept
  {
    context.post(message);
  }

  static void enter(Context &context, void *scheduler_group = nullptr) noexcept
  {
    context.enter(true, scheduler_group);
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
    context.cancel_pending();
  }

  static void wake(Context &context) noexcept;
};

} // namespace weave::detail
