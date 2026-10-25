#if os(macOS)
import Foundation

public enum LiveAudioSegment: Sendable {
    case samples([Float])
    case silence(Int)
}

/// Maps a capture device's sample clock onto a fixed host-clock frame grid. Waiting for
/// the next timestamp lets us interpolate clock drift instead of dropping samples at every
/// block boundary. Pauses and missing callbacks retain their duration as silence.
public struct LiveAudioTimeline {
    public let sampleRate: Double
    public private(set) var epoch: Int64?
    public private(set) var endFrame: Int64 = 0
    private var pending: CapturedAudioBlock?
    private let silenceMargin: Int64 = 100_000_000

    public init(sampleRate: Double) { self.sampleRate = sampleRate }

    public mutating func push(_ block: CapturedAudioBlock) -> [LiveAudioSegment] {
        guard block.frameCount > 0, block.samples.count.isMultiple(of: 2) else { return [] }
        if epoch == nil { epoch = block.hostTime }
        guard let previous = pending else { pending = block; return [] }
        guard block.hostTime > previous.hostTime else { return [] }
        pending = block
        return render(previous, until: block.hostTime, nextSamples: block.samples)
    }

    /// Called during a source pause as well as normal capture. Retains a short margin so a
    /// delayed callback can arrive before silence is committed to the append-only stream.
    public mutating func advanceSilence(to hostTime: Int64) -> [LiveAudioSegment] {
        guard let epoch else { return [] }
        var result: [LiveAudioSegment] = []
        if let block = pending {
            let end = block.hostTime + Int64(Double(block.frameCount) / sampleRate * 1e9)
            guard hostTime > end + silenceMargin else { return [] }
            pending = nil
            result += render(block, until: end, nextSamples: [])
        }
        let target = Int64((Double(hostTime - silenceMargin - epoch) * sampleRate / 1e9).rounded(.down))
        if target > endFrame {
            result.append(.silence(Int(target - endFrame)))
            endFrame = target
        }
        return result
    }

    private mutating func render(_ block: CapturedAudioBlock, until nextHost: Int64, nextSamples: [Float]) -> [LiveAudioSegment] {
        guard let epoch else { return [] }
        let start = Double(block.hostTime - epoch) * sampleRate / 1e9
        let next = Double(nextHost - epoch) * sampleRate / 1e9
        let count = block.frameCount
        let interval = next - start
        // A missing callback is a gap, not a reason to stretch the preceding audio.
        let gap = interval > Double(count) + sampleRate * 0.05
        let audioEnd = gap ? start + Double(count) : next
        var result: [LiveAudioSegment] = []
        let first = Int64(start.rounded())
        if first > endFrame {
            result.append(.silence(Int(first - endFrame)))
            endFrame = first
        }
        let last = Int64(audioEnd.rounded())
        if last > endFrame {
            let outputCount = Int(last - endFrame)
            var output = [Float](repeating: 0, count: outputCount * 2)
            let scale = gap ? 1 : Double(count) / max(interval, 1)
            for frame in 0..<outputCount {
                let position = max(0, (Double(endFrame + Int64(frame)) - start) * scale)
                let base = min(Int(position), count - 1)
                let fraction = Float(min(position - Double(base), 1))
                for channel in 0..<2 {
                    let a = block.samples[base * 2 + channel]
                    let b = base + 1 < count ? block.samples[(base + 1) * 2 + channel]
                        : (!gap && nextSamples.count >= 2 ? nextSamples[channel] : a)
                    output[frame*2+channel] = a + (b-a) * fraction
                }
            }
            result.append(.samples(output))
            endFrame = last
        }
        let nextFrame = Int64(next.rounded())
        if nextFrame > endFrame {
            result.append(.silence(Int(nextFrame - endFrame)))
            endFrame = nextFrame
        }
        return result
    }
}
#endif
