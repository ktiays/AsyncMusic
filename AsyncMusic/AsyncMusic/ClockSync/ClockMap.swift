//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

/// One NTP-style four-timestamp exchange, all values in host-clock nanoseconds.
///
/// `t0` (ping sent) and `t3` (pong received) are on the follower's clock; `t1` (ping received)
/// and `t2` (pong sent) are on the leader's clock.
nonisolated struct ClockProbe: Sendable, Equatable {
    var t0: Int64
    var t1: Int64
    var t2: Int64
    var t3: Int64

    /// Time spent on the network, excluding the leader's turnaround.
    var roundTrip: Int64 { (t3 - t0) - (t2 - t1) }

    /// Leader time minus follower time. Exact only if both legs took equally long;
    /// the error from asymmetric legs is bounded by `roundTrip / 2`.
    var offset: Int64 { ((t1 - t0) + (t2 - t3)) / 2 }

    /// Follower time at which `offset` was observed.
    var localMidpoint: Int64 { t0 + (t3 - t0) / 2 }
}

/// A linear mapping between this device's host clock and the leader's host clock:
///
///     leader = local + referenceOffset + (local - referenceLocal) * slope
///
/// `slope` is the relative clock rate difference (skew); `1e-6` means the leader's clock runs
/// one part per million faster than ours.
nonisolated struct ClockMap: Sendable, Equatable {
    var referenceLocal: Int64
    var referenceOffset: Int64
    var slope: Double

    static let identity = ClockMap(referenceLocal: 0, referenceOffset: 0, slope: 0)

    var skewPPM: Double { slope * 1e6 }

    /// Leader minus local at the given local time.
    func offset(atLocal local: Int64) -> Int64 {
        referenceOffset + Int64((Double(local - referenceLocal) * slope).rounded())
    }

    func leaderTime(fromLocal local: Int64) -> Int64 {
        local + offset(atLocal: local)
    }

    func localTime(fromLeader leader: Int64) -> Int64 {
        // Solving the mapping for `local`: with g = leader - referenceOffset,
        // local = g - (g - referenceLocal) * slope / (1 + slope).
        let guess = leader - referenceOffset
        return guess - Int64((Double(guess - referenceLocal) * slope / (1 + slope)).rounded())
    }
}
