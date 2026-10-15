#pragma once

#include <weave/io.hpp>

namespace support {

template <class F>
weave::Task<void> wait_context(weave::Context &ctx, F complete)
{
  while (!complete())
    co_await ctx.yield();
}

} // namespace support
