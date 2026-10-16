//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/host_clock.hpp"

#if defined(__APPLE__)
#include <time.h>
#elif defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace asyncmusic {

#if defined(__APPLE__)

int64_t hostClockNanoseconds() {
    return static_cast<int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

int64_t sleptNanoseconds() {
    return static_cast<int64_t>(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)) - static_cast<int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}

#elif defined(_WIN32)

int64_t hostClockNanoseconds() {
    static const int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<int64_t>(value.QuadPart);
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    const int64_t ticks = static_cast<int64_t>(counter.QuadPart);
    // Split to avoid overflowing ticks * 1e9.
    return ticks / frequency * 1'000'000'000 + ticks % frequency * 1'000'000'000 / frequency;
}

int64_t sleptNanoseconds() {
    return 0;
}

#else

namespace {
int64_t read(clockid_t clock) {
    timespec time{};
    clock_gettime(clock, &time);
    return static_cast<int64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}
}  // namespace

int64_t hostClockNanoseconds() {
    return read(CLOCK_MONOTONIC);
}

int64_t sleptNanoseconds() {
    return read(CLOCK_BOOTTIME) - read(CLOCK_MONOTONIC);
}

#endif

}  // namespace asyncmusic
