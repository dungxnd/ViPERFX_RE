// Shim: EventFlag implementation using Linux futex (no libutils dependency)
#pragma once
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <time.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "utils/Errors.h"

namespace android {
namespace hardware {

struct EventFlag {
    static status_t createEventFlag(std::atomic<uint32_t> *efWordPtr, EventFlag **ef) {
        if (!efWordPtr || !ef) return BAD_VALUE;
        *ef = new EventFlag(efWordPtr);
        return NO_ERROR;
    }

    static status_t deleteEventFlag(EventFlag **ef) {
        if (!ef || !*ef) return BAD_VALUE;
        delete *ef;
        *ef = nullptr;
        return NO_ERROR;
    }

    status_t wake(uint32_t bitmask) {
        mPtr->fetch_or(bitmask, std::memory_order_release);
        ::syscall(SYS_futex, mPtr, FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
        return NO_ERROR;
    }

    status_t wait(uint32_t bitmask, uint32_t *efState,
                  int64_t timeoutNs = 0, bool retry = false) {
        uint32_t val;
        do {
            val = mPtr->load(std::memory_order_acquire);
            if (val & bitmask) {
                *efState = val & bitmask;
                mPtr->fetch_and(~bitmask, std::memory_order_release);
                return NO_ERROR;
            }
            struct timespec ts{};
            struct timespec *tsp = nullptr;
            if (timeoutNs > 0) {
                ts.tv_sec  = timeoutNs / 1000000000LL;
                ts.tv_nsec = timeoutNs % 1000000000LL;
                tsp = &ts;
            }
            int r = ::syscall(SYS_futex, mPtr, FUTEX_WAIT_PRIVATE, val, tsp, nullptr, 0);
            if (r < 0 && errno == ETIMEDOUT) return TIMED_OUT;
        } while (retry);

        val = mPtr->load(std::memory_order_acquire);
        *efState = val & bitmask;
        return (*efState) ? NO_ERROR : static_cast<status_t>(-EAGAIN);
    }

private:
    explicit EventFlag(std::atomic<uint32_t> *p) : mPtr(p) {}
    std::atomic<uint32_t> *mPtr;
};

} // namespace hardware
} // namespace android
