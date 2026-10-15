//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Foundation
import os

/// Decoded audio the render thread can read without touching any Swift object.
nonisolated struct PCMSamples {
    var left: UnsafePointer<Float>
    var right: UnsafePointer<Float>
    var frameCount: Int
    var sampleRate: Double
}

nonisolated enum RenderSource {
    case silence
    case metronome
    case pcm(PCMSamples)
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
        case let .pcm(samples):
            sourceRate = samples.sampleRate
        }

        stats.hostTimeValid = hostTime != nil
        guard let hostTime, let map = current.clockMap, current.playback.isPlaying, sourceRate > 0 else {
            left.update(repeating: 0, count: frameCount)
            right.update(repeating: 0, count: frameCount)
            wasAudible = false
            stats.isPlaying = false
            stats.correctionPPM = 0
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
        let fadeLength = Float(max(Int(configuration.fadeDuration * outputRate), 1))
        for frame in 0 ..< frameCount {
            let readPosition = position + Double(frame) * increment
            var (l, r) = sample(at: readPosition, outputRate: outputRate)
            if fadeRemaining > 0 {
                let gain = 1 - Float(fadeRemaining) / fadeLength
                l *= gain
                r *= gain
                fadeRemaining -= 1
            }
            left[frame] = l
            right[frame] = r
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

    @inline(__always)
    private func sample(at position: Double, outputRate: Double) -> (Float, Float) {
        switch current.source {
        case .silence:
            return (0, 0)
        case .metronome:
            let value = Metronome.sample(atSeconds: position / outputRate)
            return (value, value)
        case let .pcm(samples):
            return (
                Self.interpolate(samples.left, count: samples.frameCount, at: position),
                Self.interpolate(samples.right, count: samples.frameCount, at: position)
            )
        }
    }

    /// Four-point cubic Hermite interpolation; silence outside the buffer.
    @inline(__always)
    private static func interpolate(_ samples: UnsafePointer<Float>, count: Int, at position: Double) -> Float {
        let base = position.rounded(.down)
        guard base > -2, base < Double(count) else { return 0 }
        let index = Int(base)
        let t = Float(position - base)

        @inline(__always)
        func value(_ i: Int) -> Float { i >= 0 && i < count ? samples[i] : 0 }

        let y0 = value(index - 1)
        let y1 = value(index)
        let y2 = value(index + 1)
        let y3 = value(index + 2)
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
