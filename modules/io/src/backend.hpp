#pragma once

#if defined(_WIN32)
#include "windows/iocp.hpp"
#else
#include "linux/uring.hpp"
#endif
