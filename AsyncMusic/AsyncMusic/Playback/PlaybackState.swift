//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

/// What the group should be playing, expressed on the leader's clock.
///
/// Media time advances one-for-one with leader time from the anchor, so every device can work
/// out which instant of the source belongs to any moment, whatever its own sample rate:
///
///     mediaTime(leaderTime) = startMediaTime + (leaderTime - startLeaderTime)
nonisolated struct PlaybackState: Sendable, Equatable {
    /// Identifies the audio source; `PlaybackSource.none` means stopped.
    var source: UInt64
    /// Leader host time (nanoseconds) at which `startMediaTime` is heard.
    var startLeaderTime: Int64
    /// Position in the source (nanoseconds) heard at `startLeaderTime`. Negative values are
    /// allowed and simply mean silence until the source reaches zero.
    var startMediaTime: Int64

    static let stopped = PlaybackState(source: PlaybackSource.none, startLeaderTime: 0, startMediaTime: 0)

    var isPlaying: Bool { source != PlaybackSource.none }

    func mediaTime(atLeaderTime leaderTime: Int64) -> Int64 {
        startMediaTime + (leaderTime - startLeaderTime)
    }
}

nonisolated enum PlaybackSource {
    static let none: UInt64 = 0
    static let metronome: UInt64 = 1
}
