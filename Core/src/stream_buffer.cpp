//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/stream_buffer.hpp"

#include <algorithm>
#include <cassert>

#include "asyncmusic/protocol.hpp"

namespace asyncmusic {

StreamBuffer::StreamBuffer(double sampleRate, int capacity, int64_t startFrame)
    : sampleRate_(sampleRate),
      capacity_(capacity),
      left_(static_cast<size_t>(capacity), 0.0f),
      right_(static_cast<size_t>(capacity), 0.0f),
      start_(startFrame),
      end_(startFrame) {
    assert(capacity > 0);
}

void StreamBuffer::append(const float* left, const float* right, int count) {
    if (count <= 0) return;
    int remaining = std::min(count, capacity_);
    int offset = count - remaining;
    int64_t frame = end_.load(std::memory_order_relaxed) + offset;
    const int64_t newEnd = frame + remaining;

    if (newEnd - start_.load(std::memory_order_relaxed) > capacity_) {
        start_.store(newEnd - capacity_, std::memory_order_seq_cst);
        // Readers must observe the new start before any of the overwritten samples.
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    while (remaining > 0) {
        const int index = slot(frame);
        const int run = std::min(remaining, capacity_ - index);
        std::copy(left + offset, left + offset + run, left_.begin() + index);
        std::copy(right + offset, right + offset + run, right_.begin() + index);
        remaining -= run;
        offset += run;
        frame += run;
    }
    end_.store(newEnd, std::memory_order_release);
}

FrameRange StreamBuffer::validRange() const {
    const int64_t upper = end_.load(std::memory_order_acquire);
    const int64_t lower = start_.load(std::memory_order_acquire);
    return FrameRange{lower, std::max(lower, upper)};
}

bool StreamBuffer::isStillValid(int64_t frame) const {
    std::atomic_thread_fence(std::memory_order_acquire);
    return start_.load(std::memory_order_acquire) <= frame;
}

bool StreamBuffer::readInterleavedPCM16(int64_t frame, int count, int16_t* destination) const {
    const FrameRange range = validRange();
    if (count <= 0 || frame < range.lowerBound || frame + count > range.upperBound) return false;
    for (int i = 0; i < count; ++i) {
        const StereoSample sample = sampleAt(frame + i);
        destination[2 * i] = toPCM16(sample.left);
        destination[2 * i + 1] = toPCM16(sample.right);
    }
    return isStillValid(frame);
}

}  // namespace asyncmusic
