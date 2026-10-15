//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cmath>
#include <cstdint>

#include "config.hpp"

namespace asyncmusic {

/// One NTP-style four-timestamp exchange, all values in host-clock nanoseconds.
///
/// `t0` (ping sent) and `t3` (pong received) are on the follower's clock; `t1` (ping received)
/// and `t2` (pong sent) are on the leader's clock.
struct ClockProbe {
    int64_t t0 = 0;
    int64_t t1 = 0;
    int64_t t2 = 0;
    int64_t t3 = 0;

    /// Time spent on the network, excluding the leader's turnaround.
    int64_t roundTrip() const { return (t3 - t0) - (t2 - t1); }

    /// Leader time minus follower time. Exact only if both legs took equally long; the error
    /// from asymmetric legs is bounded by `roundTrip() / 2`.
    int64_t offset() const { return ((t1 - t0) + (t2 - t3)) / 2; }

    /// Follower time at which `offset()` was observed.
    int64_t localMidpoint() const { return t0 + (t3 - t0) / 2; }

    bool operator==(const ClockProbe& other) const {
        return t0 == other.t0 && t1 == other.t1 && t2 == other.t2 && t3 == other.t3;
    }
};

/// A linear mapping between this device's host clock and the leader's host clock:
///
///     leader = local + referenceOffset + (local - referenceLocal) * slope
///
/// `slope` is the relative clock rate difference (skew); `1e-6` means the leader's clock runs
/// one part per million faster than ours.
struct ClockMap {
    int64_t referenceLocal = 0;
    int64_t referenceOffset = 0;
    double slope = 0;

    static ClockMap identity() { return ClockMap{}; }

    double skewPPM() const { return slope * 1e6; }

    /// Leader minus local at the given local time.
    int64_t offsetAtLocal(int64_t local) const {
        return referenceOffset + static_cast<int64_t>(std::llround(static_cast<double>(local - referenceLocal) * slope));
    }

    int64_t leaderTimeFromLocal(int64_t local) const { return local + offsetAtLocal(local); }

    int64_t localTimeFromLeader(int64_t leader) const {
        // Solving the mapping for `local`: with g = leader - referenceOffset,
        // local = g - (g - referenceLocal) * slope / (1 + slope).
        const int64_t guess = leader - referenceOffset;
        return guess - static_cast<int64_t>(std::llround(static_cast<double>(guess - referenceLocal) * slope / (1 + slope)));
    }

    bool operator==(const ClockMap& other) const {
        return referenceLocal == other.referenceLocal && referenceOffset == other.referenceOffset && slope == other.slope;
    }
};

}  // namespace asyncmusic
