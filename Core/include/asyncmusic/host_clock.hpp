//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstdint>

namespace asyncmusic {

/// The device-local monotonic clock every sync calculation is expressed in, in nanoseconds.
///
/// It must be the clock the platform's audio timestamps use, so a network-synced time maps
/// directly onto when a buffer is heard:
///
/// - Apple: `CLOCK_UPTIME_RAW` (= `mach_absolute_time`, as in `AudioTimeStamp.mHostTime`)
/// - Android / Linux: `CLOCK_MONOTONIC` (AAudio, ALSA and PipeWire timestamps)
/// - Windows: `QueryPerformanceCounter` (WASAPI `IAudioClock::GetPosition`)
int64_t hostClockNanoseconds();

/// Total time the device has spent suspended since boot, in nanoseconds.
///
/// Where the host clock stops during sleep (Apple, Android, Linux), a change in this value
/// between two readings means the host clock fell behind every other device's by that much,
/// and any mapping to another device's clock is void. On Windows the host clock keeps
/// counting through sleep, so this stays constant.
int64_t sleptNanoseconds();

/// Changes in `sleptNanoseconds()` smaller than this are just the clock reads' jitter.
constexpr int64_t kSleepDetectionThreshold = 1'000'000;

}  // namespace asyncmusic
