// Shim: status_t and common POSIX-based error codes
#pragma once
#include <cerrno>
#include <cstdint>
namespace android {
using status_t = int32_t;
constexpr status_t NO_ERROR            =  0;
constexpr status_t BAD_VALUE           = -EINVAL;
constexpr status_t NO_MEMORY           = -ENOMEM;
constexpr status_t TIMED_OUT           = -ETIMEDOUT;
constexpr status_t INVALID_OPERATION   = -ENOSYS;
} // namespace android
