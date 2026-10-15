//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

import Synchronization

/// A fixed-size ring of stereo samples, addressed by absolute frame number in the stream.
///
/// One writer (a decoder now, the network later) appends contiguous chunks; the render thread
/// reads whatever frames it needs that are still held. The buffer always holds one contiguous
/// range `[start, end)`; frames outside it read as silence. Nothing here blocks or allocates
/// after `init`, so the reader is real-time safe.
///
/// When an append would exceed the capacity, the writer first moves `start` forward and only
/// then overwrites the oldest frames. A reader checks `start` again after reading (see
/// `isStillValid(from:)`), so in the unlikely case it raced with an overwrite it can discard
/// what it read instead of playing garbage. In practice the writer stays a few seconds ahead
/// of playback and the overwritten frames are seconds in the past.
nonisolated final class StreamBuffer: @unchecked Sendable {
    let sampleRate: Double
    let capacity: Int

    private let left: UnsafeMutablePointer<Float>
    private let right: UnsafeMutablePointer<Float>
    private let start: Atomic<Int64>
    private let end: Atomic<Int64>

    /// - Parameter startFrame: Frame number of the first frame that will be appended.
    init(sampleRate: Double, capacity: Int, startFrame: Int64) {
        precondition(capacity > 0)
        self.sampleRate = sampleRate
        self.capacity = capacity
        left = .allocate(capacity: capacity)
        left.initialize(repeating: 0, count: capacity)
        right = .allocate(capacity: capacity)
        right.initialize(repeating: 0, count: capacity)
        start = Atomic(startFrame)
        end = Atomic(startFrame)
    }

    deinit {
        left.deallocate()
        right.deallocate()
    }

    // MARK: - Writer

    /// One past the last frame appended so far.
    var endFrame: Int64 {
        end.load(ordering: .acquiring)
    }

    /// Appends `count` frames right after `endFrame`. Single writer only.
    func append(left source: UnsafePointer<Float>, right sourceRight: UnsafePointer<Float>, count: Int) {
        var remaining = min(count, capacity)
        var offset = count - remaining
        var frame = end.load(ordering: .relaxed) + Int64(offset)
        let newEnd = frame + Int64(remaining)

        if newEnd - start.load(ordering: .relaxed) > Int64(capacity) {
            start.store(newEnd - Int64(capacity), ordering: .sequentiallyConsistent)
            // Readers must observe the new start before any of the overwritten samples.
            atomicMemoryFence(ordering: .sequentiallyConsistent)
        }

        while remaining > 0 {
            let index = slot(of: frame)
            let run = min(remaining, capacity - index)
            (left + index).update(from: source + offset, count: run)
            (right + index).update(from: sourceRight + offset, count: run)
            remaining -= run
            offset += run
            frame += Int64(run)
        }
        end.store(newEnd, ordering: .releasing)
    }

    // MARK: - Reader

    /// The frames currently held, `start ..< end`.
    func validRange() -> Range<Int64> {
        let upper = end.load(ordering: .acquiring)
        let lower = start.load(ordering: .acquiring)
        return lower ..< max(lower, upper)
    }

    /// Unchecked read; `frame` must lie in a range returned by `validRange()`.
    @inline(__always)
    func sample(at frame: Int64) -> (Float, Float) {
        let index = slot(of: frame)
        return (left[index], right[index])
    }

    /// Whether frames from `frame` on, read since the last `validRange()`, cannot have been
    /// overwritten in the meantime.
    func isStillValid(from frame: Int64) -> Bool {
        atomicMemoryFence(ordering: .acquiring)
        return start.load(ordering: .acquiring) <= frame
    }

    @inline(__always)
    private func slot(of frame: Int64) -> Int {
        let remainder = Int(frame % Int64(capacity))
        return remainder < 0 ? remainder + capacity : remainder
    }
}
