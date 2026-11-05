//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import AsyncMusicCore
import Foundation

// The sync and playback logic lives in the cross-platform C++ core (`Core/` at the repository
// root). These aliases and small conveniences let the Apple layer use it with Swift idioms:
// argument labels, optionals and ranges where the C++ API uses flags and out parameters.

typealias ClockProbe = asyncmusic.ClockProbe
typealias ClockMap = asyncmusic.ClockMap
typealias ClockEstimator = asyncmusic.ClockEstimator
typealias ClockVerdict = asyncmusic.ClockVerdict
typealias PlaybackState = asyncmusic.PlaybackState
typealias PlaybackPosition = asyncmusic.PlaybackPosition
typealias StreamBuffer = asyncmusic.StreamBuffer
typealias SyncedRenderer = asyncmusic.SyncedRenderer
typealias RenderStats = asyncmusic.RenderStats
typealias RenderSourceKind = asyncmusic.RenderSourceKind
typealias SpeakerLayout = asyncmusic.SpeakerLayout
typealias SpeakerChannel = asyncmusic.SpeakerChannel
typealias ChannelMix = asyncmusic.ChannelMix

nonisolated enum PlaybackSource {
    static let none = asyncmusic.kPlaybackSourceNone
    static let metronome = asyncmusic.kPlaybackSourceMetronome
    static let channelTest = asyncmusic.kPlaybackSourceChannelTest
    /// File IDs are kept at or above this.
    static let firstFile = asyncmusic.kPlaybackSourceFirstFile

    /// Sources rendered on every device from the clock alone, without a stream.
    static func isGenerated(_ source: UInt64) -> Bool {
        source == metronome || source == channelTest
    }
}

// The C++ types define `operator==`; these adopt it.
extension asyncmusic.ClockProbe: @retroactive Equatable {}
extension asyncmusic.ClockMap: @retroactive Equatable {}
extension asyncmusic.PlaybackState: @retroactive Equatable {}
extension asyncmusic.ChannelMix: @retroactive Equatable {}

// Plain values, or objects that synchronize internally.
extension asyncmusic.ClockProbe: @retroactive @unchecked Sendable {}
extension asyncmusic.ClockMap: @retroactive @unchecked Sendable {}
extension asyncmusic.PlaybackState: @retroactive @unchecked Sendable {}
extension asyncmusic.RenderStats: @retroactive @unchecked Sendable {}
extension asyncmusic.ChannelMix: @retroactive @unchecked Sendable {}
extension asyncmusic.StreamBuffer: @retroactive @unchecked Sendable {}
extension asyncmusic.PlaybackPosition: @retroactive @unchecked Sendable {}
extension asyncmusic.SyncedRenderer: @retroactive @unchecked Sendable {}

nonisolated extension asyncmusic.ClockMap {
    func offset(atLocal local: Int64) -> Int64 { offsetAtLocal(local) }
    func leaderTime(fromLocal local: Int64) -> Int64 { leaderTimeFromLocal(local) }
    func localTime(fromLeader leader: Int64) -> Int64 { localTimeFromLeader(leader) }
}

nonisolated extension asyncmusic.PlaybackState {
    func mediaTime(atLeaderTime leaderTime: Int64) -> Int64 { mediaTimeAtLeaderTime(leaderTime) }
}

nonisolated extension Data {
    /// Copies the core's byte vector through its storage rather than its `Sequence`
    /// conformance, which faults in some runtimes (seen in iOS 26.5 Simulator builds).
    init(_ bytes: asyncmusic.Bytes) {
        let count = Int(bytes.size())
        if count == 0 {
            self.init()
        } else {
            self.init(bytes: bytes.__dataUnsafe()!, count: count)
        }
    }

    /// A copy as the core's byte vector.
    var coreBytes: asyncmusic.Bytes {
        withUnsafeBytes { asyncmusic.makeBytes($0.bindMemory(to: UInt8.self).baseAddress, $0.count) }
    }
}

nonisolated extension asyncmusic.SpeakerLayout {
    /// The channels this layout splits the music into, one device each.
    var channels: [SpeakerChannel] {
        (0 ..< asyncmusic.speakerLayoutChannelCount(self)).map { asyncmusic.speakerLayoutChannel(self, $0) }
    }
}

nonisolated extension asyncmusic.ClockVerdict {
    var name: String {
        switch self {
        case .accepted: "accepted"
        case .noReplies: "no replies"
        case .slowRoundTrip: "slow round trip"
        case .outlier: "outlier"
        case .reset: "reset"
        @unknown default: "unknown"
        }
    }
}

nonisolated extension asyncmusic.ClockEstimator {
    /// Feeds one round of probes; only the fastest one is used.
    mutating func addRound(_ probes: [ClockProbe]) -> ClockVerdict {
        probes.withUnsafeBufferPointer { addRound($0.baseAddress, $0.count) }
    }

    /// The fitted mapping, once there is one.
    var fittedMap: ClockMap? { hasMap() ? map() : nil }

    /// Lowest recent round trip, once any probe was accepted or rejected.
    var lowestRoundTrip: Int64? {
        let value = minimumRoundTrip()
        return value > 0 ? value : nil
    }
}

nonisolated extension asyncmusic.StreamBuffer {
    static func make(sampleRate: Double, capacity: Int, startFrame: Int64) -> StreamBuffer {
        create(sampleRate, Int32(capacity), startFrame)
    }

    /// The frames currently held.
    var heldFrames: Range<Int64> {
        let range = validRange()
        return range.lowerBound ..< range.upperBound
    }

    func append(left: UnsafePointer<Float>, right: UnsafePointer<Float>, count: Int) {
        append(left, right, Int32(count))
    }
}

nonisolated extension asyncmusic.PlaybackPosition {
    /// The media time (nanoseconds) being heard now, or nil while the clock map is unknown.
    func heardMediaTime() -> Int64? {
        var mediaTime: Int64 = 0
        return heardMediaTime(HostClock.now(), &mediaTime) ? mediaTime : nil
    }

    func setClockMap(_ map: ClockMap?) {
        if let map {
            setClockMap(map)
        } else {
            clearClockMap()
        }
    }
}

nonisolated extension asyncmusic.SyncedRenderer {
    func setClockMap(_ map: ClockMap?) {
        if let map {
            setClockMap(map)
        } else {
            clearClockMap()
        }
    }
}
