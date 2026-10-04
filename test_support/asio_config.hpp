#pragma once
#include <weave/core.hpp>
#include <asio.hpp>
#include <cstdio>

namespace asio::detail {

template <class Exception>
void throw_exception(const Exception &)
{
  std::fputs("Fatal Asio failure (exceptions disabled)\n", stderr);
  std::abort();
}

} // namespace asio::detail
