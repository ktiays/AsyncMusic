//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "config.hpp"
#include "ref_counted.hpp"

AM_ASSUME_NONNULL_BEGIN

namespace asyncmusic {

/// A half-open range of frame numbers, `[lowerBound, upperBound)`.
struct FrameRange {
    int64_t lowerBound = 0;
    int64_t upperBound = 0;

    bool contains(int64_t frame) const { return frame >= lowerBound && frame < upperBound; }
    int64_t count() const { return upperBound - lowerBound; }
};

struct StereoSample {
    float left = 0;
    float right = 0;
};

class StreamBuffer;
}  // namespace asyncmusic

void asyncmusic_stream_buffer_retain(asyncmusic::StreamBuffer* object);
void asyncmusic_stream_buffer_release(asyncmusic::StreamBuffer* object);

namespace asyncmusic {

/// A fixed-size ring of stereo samples, addressed by absolute frame number in the stream.
///
/// One writer (a decoder or the network) appends contiguous chunks; the render thread reads
/// whatever frames it needs that are still held. The buffer always holds one contiguous range
/// `[start, end)`; frames outside it read as silence. Nothing here blocks or allocates after
/// `create`, so readers are real-time safe.
///
/// When an append would exceed the capacity, the writer first moves `start` forward and only
/// then overwrites the oldest frames. A reader checks `start` again after reading (see
/// `isStillValid`), so in the unlikely case it raced with an overwrite it can discard what it
/// read instead of playing garbage. In practice the writer stays a few seconds ahead of
/// playback and the overwritten frames are seconds in the past.
class StreamBuffer final : public RefCounted {
public:
    /// `startFrame` is the frame number of the first frame that will be appended.
    static StreamBuffer* create(double sampleRate, int capacity, int64_t startFrame) SWIFT_RETURNS_RETAINED {
        return new StreamBuffer(sampleRate, capacity, startFrame);
    }

    double sampleRate() const { return sampleRate_; }
    int capacity() const { return capacity_; }

    // Writer (one thread at a time)

    /// One past the last frame appended so far.
    int64_t endFrame() const { return end_.load(std::memory_order_acquire); }

    /// Appends `count` frames right after `endFrame()`.
    void append(const float* left, const float* right, int count);

    // Readers (any thread, including real-time audio)

    /// The frames currently held.
    FrameRange validRange() const;

    /// Unchecked read; `frame` must lie in a range returned by `validRange()`.
    StereoSample sampleAt(int64_t frame) const {
        const int index = slot(frame);
        return StereoSample{left_[static_cast<size_t>(index)], right_[static_cast<size_t>(index)]};
    }

    /// Whether frames from `frame` on, read since the last `validRange()`, cannot have been
    /// overwritten in the meantime.
    bool isStillValid(int64_t frame) const;

    /// Copies `count` frames from `frame` as interleaved 16-bit stereo (the stream wire
    /// format), clamped to the held range. Returns false if any of them were not held or were
    /// overwritten during the copy.
    bool readInterleavedPCM16(int64_t frame, int count, int16_t* destination) const;

private:
    StreamBuffer(double sampleRate, int capacity, int64_t startFrame);

    int slot(int64_t frame) const {
        const int remainder = static_cast<int>(frame % capacity_);
        return remainder < 0 ? remainder + capacity_ : remainder;
    }

    const double sampleRate_;
    const int capacity_;
    std::vector<float> left_;
    std::vector<float> right_;
    std::atomic<int64_t> start_;
    std::atomic<int64_t> end_;
} SWIFT_SHARED_REFERENCE(asyncmusic_stream_buffer_retain, asyncmusic_stream_buffer_release);

}  // namespace asyncmusic

inline void asyncmusic_stream_buffer_retain(asyncmusic::StreamBuffer* object) { object->retain(); }
inline void asyncmusic_stream_buffer_release(asyncmusic::StreamBuffer* object) { object->release(); }

AM_ASSUME_NONNULL_END
