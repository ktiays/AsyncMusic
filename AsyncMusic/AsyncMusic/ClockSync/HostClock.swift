//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Darwin

/// The device-local monotonic clock that every sync calculation is expressed in.
///
/// This is the same clock as `mach_absolute_time()`, which Core Audio stamps into
/// `AudioTimeStamp.mHostTime`, so a network-synced time maps directly onto audio render
/// timestamps. `CLOCK_UPTIME_RAW` just reports it pre-scaled to nanoseconds.
nonisolated enum HostClock {
    /// Current host time in nanoseconds.
    static func now() -> Int64 {
        Int64(clock_gettime_nsec_np(CLOCK_UPTIME_RAW))
    }

    /// Total time the device has spent asleep since boot, in nanoseconds.
    ///
    /// The host clock stops while the device sleeps, so if this changes between two readings
    /// the host clock fell behind every other device's by that much and any mapping to another
    /// device's host clock is void.
    static func sleepDuration() -> Int64 {
        Int64(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)) - Int64(clock_gettime_nsec_np(CLOCK_UPTIME_RAW))
    }

    /// Changes in `sleepDuration()` smaller than this are just the two clock reads' jitter.
    static let sleepDetectionThreshold: Int64 = 1_000_000

    private static let timebase: mach_timebase_info_data_t = {
        var info = mach_timebase_info_data_t()
        mach_timebase_info(&info)
        return info
    }()

    /// Converts `mach_absolute_time()` ticks (e.g. `AudioTimeStamp.mHostTime`) to nanoseconds.
    static func nanoseconds(fromHostTicks ticks: UInt64) -> Int64 {
        Int64(ticks * UInt64(timebase.numer) / UInt64(timebase.denom))
    }

    /// Converts nanoseconds back to `mach_absolute_time()` ticks.
    static func hostTicks(fromNanoseconds nanoseconds: Int64) -> UInt64 {
        UInt64(nanoseconds) * UInt64(timebase.denom) / UInt64(timebase.numer)
    }
}
