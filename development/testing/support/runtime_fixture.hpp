#pragma once

#include <weave/runtime.hpp>
#include <type_traits>
#include <array>

namespace support {

using ShardedIo = std::integral_constant<weave::IoLayout, weave::IoLayout::sharded>;
using SharedIo = std::integral_constant<weave::IoLayout, weave::IoLayout::shared>;

#if defined(_WIN32)
#define WEAVE_TEST_IO_LAYOUTS support::ShardedIo, support::SharedIo
inline constexpr std::array io_layouts{weave::IoLayout::sharded, weave::IoLayout::shared};
#else
#define WEAVE_TEST_IO_LAYOUTS support::ShardedIo
inline constexpr std::array io_layouts{weave::IoLayout::sharded};
#endif

template <class Layout>
static weave::Result<weave::Runtime> create_runtime(weave::RuntimeOptions options = {}) noexcept
{
  options.io_layout = Layout::value;
  return weave::Runtime::create(options);
}

} // namespace support
