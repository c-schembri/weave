#pragma once

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#else
#include <cerrno>
#endif

namespace support {

#if defined(_WIN32)
inline constexpr int native_cancelled = ERROR_OPERATION_ABORTED;
inline constexpr int native_not_socket = WSAENOTSOCK;
inline constexpr int native_reset = WSAECONNRESET;
#else
inline constexpr int native_cancelled = ECANCELED;
inline constexpr int native_not_socket = EBADF;
inline constexpr int native_reset = ECONNRESET;
#endif

} // namespace support
