// Shim: nsecs_t and systemTime() via POSIX clock_gettime
#pragma once
#include <cstdint>
#include <time.h>
namespace android {
using nsecs_t = int64_t;
inline nsecs_t systemTime(int clock = CLOCK_MONOTONIC) {
    struct timespec ts{};
    clock_gettime(clock, &ts);
    return static_cast<nsecs_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}
} // namespace android
