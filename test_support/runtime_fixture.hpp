#pragma once

#include <weave/runtime.hpp>
#include <type_traits>

namespace support {

using ShardedIo = std::integral_constant<weave::IoLayout, weave::IoLayout::sharded>;
using SharedIo = std::integral_constant<weave::IoLayout, weave::IoLayout::shared>;

template <class Layout>
static weave::Result<weave::Runtime> create_runtime(weave::RuntimeOptions options = {}) noexcept
{
  options.io_layout = Layout::value;
  return weave::Runtime::create(options);
}

} // namespace support
