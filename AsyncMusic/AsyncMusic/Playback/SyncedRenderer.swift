//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
import os

nonisolated enum RenderSource {
    case silence
    case metronome
    /// Unretained so copying the control state on the render thread never touches reference
    /// counts. Whoever installs it must keep the buffer alive until the render thread can no
    /// longer be using it (see `SyncedPlayer`).
    case stream(Unmanaged<StreamBuffer>)
}

/// Snapshot of what the render thread is doing, for the debug UI.
nonisolated struct RenderStats: Sendable {
    var hostTimeValid = true
    var isPlaying = false
    /// Where the output should be minus where it is, in microseconds (before correction).
    var errorMicroseconds: Double = 0
    /// Current playback-rate correction, in parts per million.
    var correctionPPM: Double = 0
    /// Number of times playback was repositioned instead of smoothly corrected.
    var jumps = 0
    /// Source position currently being output, in seconds.
    var mediaSeconds: Double = 0
    /// How far the stream buffer reaches beyond the current position, in seconds.
    var bufferedSeconds: Double = 0
}

/// Works out, for every render cycle, which part of the source must be heard when that buffer
/// reaches the speaker, and plays it.
///
/// Rather than starting playback once and hoping it stays aligned, each cycle maps the buffer's
/// presentation time onto the leader's clock and from there onto media time. Small differences
/// between that target and the running read position (clock-map updates, DAC crystal drift)
/// are absorbed by a PI controller nudging the playback rate by up to `maximumCorrection`; the
/// integral term learns the DAC's steady rate error so it is tracked without a lag. Large
/// differences (start, seek, a big re-estimate) reposition the read head directly.
///
/// `render` runs on the real-time audio thread: it never blocks, allocates or retains. Control
/// state comes in through `update`, which the render thread picks up with a try-lock and
/// otherwise keeps using its previous copy.
nonisolated final class SyncedRenderer: @unchecked Sendable {
    struct Configuration {
        /// Proportional time constant of the rate controller, in seconds. The integral time
        /// constant is four times this, which makes the loop critically damped.
        var correctionTimeConstant = 1.0
        /// 1000 ppm is a 1.7 cent pitch change, below what anyone can hear.
        var maximumCorrection = 1000e-6
        /// Errors beyond this many seconds reposition the read head instead.
        var jumpThreshold = 0.010
        /// Length of the fade-in after repositioning mid-stream, in seconds.
        var fadeDuration = 0.005
    }

    struct ControlState {
        var clockMap: ClockMap?
        var playback = PlaybackState.stopped
        /// Changes whenever `playback` changes, so the render thread knows to reposition.
        var playbackGeneration = 0
        var source = RenderSource.silence
        /// Host time from the render timestamp to the sound leaving the device, plus the user's
        /// manual offset, in nanoseconds.
        var presentationDelay: Int64 = 0
    }

    let configuration: Configuration

    private let control = OSAllocatedUnfairLock(uncheckedState: ControlState())
    private let statsLock = OSAllocatedUnfairLock(initialState: RenderStats())

    // Render-thread-only state.
    private var current = ControlState()
    private var position = 0.0
    private var lastGeneration = -1
    private var wasAudible = false
    private var fadeRemaining = 0
    /// Integral term of the rate controller: the learned steady-state rate error.
    private var integral = 0.0
    private var stats = RenderStats()

    init(configuration: Configuration = Configuration()) {
        self.configuration = configuration
    }

    func update(_ body: (inout ControlState) -> Void) {
        control.withLockUnchecked { body(&$0) }
    }

    var latestStats: RenderStats {
        statsLock.withLock { $0 }
    }

    /// Fills `frameCount` frames. `hostTime` is the render timestamp's host time in
    /// nanoseconds, or nil when the timestamp carries none.
    func render(
        hostTime: Int64?,
        frameCount: Int,
        outputRate: Double,
        left: UnsafeMutablePointer<Float>,
        right: UnsafeMutablePointer<Float>
    ) {
        if let fresh = control.withLockIfAvailableUnchecked({ $0 }) {
            current = fresh
        }

        let sourceRate: Double
        switch current.source {
        case .silence:
            sourceRate = 0
        case .metronome:
            sourceRate = outputRate
        case let .stream(buffer):
            sourceRate = buffer._withUnsafeGuaranteedRef { $0.sampleRate }
        }

        stats.hostTimeValid = hostTime != nil
        guard let hostTime, let map = current.clockMap, current.playback.isPlaying, sourceRate > 0 else {
            left.update(repeating: 0, count: frameCount)
            right.update(repeating: 0, count: frameCount)
            wasAudible = false
            stats.isPlaying = false
            stats.correctionPPM = 0
            stats.bufferedSeconds = 0
            publishStats()
            return
        }

        // Which instant of the source must be heard when this buffer's first frame is?
        let audibleAt = hostTime + current.presentationDelay
        let mediaTime = current.playback.mediaTime(atLeaderTime: map.leaderTime(fromLocal: audibleAt))
        let target = Double(mediaTime) * sourceRate / 1e9
        let error = target - position

        var correction = 0.0
        if current.playbackGeneration != lastGeneration || abs(error) > configuration.jumpThreshold * sourceRate {
            if wasAudible {
                fadeRemaining = Int(configuration.fadeDuration * outputRate)
            }
            if current.playbackGeneration == lastGeneration {
                stats.jumps += 1
            }
            lastGeneration = current.playbackGeneration
            position = target
        } else {
            let limit = configuration.maximumCorrection
            let errorSeconds = error / sourceRate
            let timeConstant = configuration.correctionTimeConstant
            let elapsed = Double(frameCount) / outputRate
            integral += errorSeconds / (4 * timeConstant * timeConstant) * elapsed
            integral = min(max(integral, -limit), limit)
            correction = min(max(errorSeconds / timeConstant + integral, -limit), limit)
        }

        let increment = sourceRate / outputRate * (1 + correction)
        switch current.source {
        case .silence:
            left.update(repeating: 0, count: frameCount)
            right.update(repeating: 0, count: frameCount)
        case .metronome:
            for frame in 0 ..< frameCount {
                let value = Metronome.sample(atSeconds: (position + Double(frame) * increment) / outputRate)
                left[frame] = value
                right[frame] = value
            }
        case let .stream(buffer):
            buffer._withUnsafeGuaranteedRef { buffer in
                let range = buffer.validRange()
                for frame in 0 ..< frameCount {
                    let (l, r) = Self.interpolate(buffer, range: range, at: position + Double(frame) * increment)
                    left[frame] = l
                    right[frame] = r
                }
                // The writer only overwrites frames far behind playback, but if it ever lapped
                // us mid-read, play silence rather than whatever it wrote there.
                let oldestRead = max(range.lowerBound, Int64(position.rounded(.down)) - 1)
                if !buffer.isStillValid(from: oldestRead) {
                    left.update(repeating: 0, count: frameCount)
                    right.update(repeating: 0, count: frameCount)
                }
                stats.bufferedSeconds = max(0, Double(range.upperBound) - position) / sourceRate
            }
        }

        if fadeRemaining > 0 {
            let fadeLength = Float(max(Int(configuration.fadeDuration * outputRate), 1))
            for frame in 0 ..< min(fadeRemaining, frameCount) {
                let gain = 1 - Float(fadeRemaining - frame) / fadeLength
                left[frame] *= gain
                right[frame] *= gain
            }
            fadeRemaining = max(0, fadeRemaining - frameCount)
        }
        position += Double(frameCount) * increment
        wasAudible = true

        stats.isPlaying = true
        stats.errorMicroseconds = error / sourceRate * 1e6
        stats.correctionPPM = correction * 1e6
        stats.mediaSeconds = position / sourceRate
        publishStats()
    }

    private func publishStats() {
        let snapshot = stats
        statsLock.withLockIfAvailable { $0 = snapshot }
    }

    /// Four-point cubic Hermite interpolation; silence outside `range`.
    @inline(__always)
    private static func interpolate(_ buffer: StreamBuffer, range: Range<Int64>, at position: Double) -> (Float, Float) {
        let base = position.rounded(.down)
        let index = Int64(base)
        guard index + 2 >= range.lowerBound, index - 1 < range.upperBound else { return (0, 0) }
        let t = Float(position - base)

        @inline(__always)
        func value(_ frame: Int64) -> (Float, Float) {
            range.contains(frame) ? buffer.sample(at: frame) : (0, 0)
        }

        let y0 = value(index - 1)
        let y1 = value(index)
        let y2 = value(index + 1)
        let y3 = value(index + 2)
        return (
            hermite(y0.0, y1.0, y2.0, y3.0, t),
            hermite(y0.1, y1.1, y2.1, y3.1, t)
        )
    }

    @inline(__always)
    private static func hermite(_ y0: Float, _ y1: Float, _ y2: Float, _ y3: Float, _ t: Float) -> Float {
        let c1 = 0.5 * (y2 - y0)
        let c2 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3
        let c3 = 0.5 * (y3 - y0) + 1.5 * (y1 - y2)
        return ((c3 * t + c2) * t + c1) * t + y1
    }
}

/// A click track computed directly from media time, so every device produces exactly the same
/// signal at the same media instant regardless of sample rate. Sharp attacks make any timing
/// difference between devices easy to hear as a flam.
nonisolated enum Metronome {
    static let beatsPerMinute = 120.0
    static let beatsPerBar = 4

    static func sample(atSeconds seconds: Double) -> Float {
        guard seconds >= 0 else { return 0 }
        let period = 60 / beatsPerMinute
        let beat = (seconds / period).rounded(.down)
        let sinceBeat = seconds - beat * period
        guard sinceBeat < 0.08 else { return 0 }
        let frequency = Int(beat) % beatsPerBar == 0 ? 1760.0 : 880.0
        let attack = min(1, sinceBeat / 0.0005)
        let envelope = attack * exp(-sinceBeat / 0.012)
        return Float(0.5 * envelope * sin(2 * .pi * frequency * sinceBeat))
    }
}
