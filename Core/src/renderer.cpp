//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/renderer.hpp"

#include <algorithm>
#include <cmath>

namespace asyncmusic {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Four-point cubic Hermite interpolation.
inline float hermite(float y0, float y1, float y2, float y3, float t) {
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2 * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

/// Interpolates the stream at a fractional frame position; silence outside `range`.
inline StereoSample interpolate(const StreamBuffer& buffer, const FrameRange& range, double position) {
    const double base = std::floor(position);
    const auto index = static_cast<int64_t>(base);
    if (index + 2 < range.lowerBound || index - 1 >= range.upperBound) return {};
    const auto t = static_cast<float>(position - base);
    auto value = [&](int64_t frame) { return range.contains(frame) ? buffer.sampleAt(frame) : StereoSample{}; };
    const StereoSample y0 = value(index - 1), y1 = value(index), y2 = value(index + 1), y3 = value(index + 2);
    return StereoSample{
        hermite(y0.left, y1.left, y2.left, y3.left, t),
        hermite(y0.right, y1.right, y2.right, y3.right, t),
    };
}

/// One click `sinceBeat` seconds after its beat; silence once it has died away.
float click(double sinceBeat, double frequency) {
    if (sinceBeat < 0 || sinceBeat >= 0.08) return 0;
    const double attack = std::min(1.0, sinceBeat / 0.0005);
    const double envelope = attack * std::exp(-sinceBeat / 0.012);
    return static_cast<float>(0.5 * envelope * std::sin(2 * kPi * frequency * sinceBeat));
}

constexpr double kBeatPeriod = 60.0 / 120;  // 120 bpm

}  // namespace

float metronomeSample(double seconds) {
    constexpr int beatsPerBar = 4;
    if (seconds < 0) return 0;
    const double beat = std::floor(seconds / kBeatPeriod);
    const double frequency = static_cast<int64_t>(beat) % beatsPerBar == 0 ? 1760.0 : 880.0;
    return click(seconds - beat * kBeatPeriod, frequency);
}

StereoSample channelTestSample(double seconds) {
    if (seconds < 0) return {};
    const double beat = std::floor(seconds / kBeatPeriod);
    const bool isRight = static_cast<int64_t>(beat) % 2 == 1;
    const float value = click(seconds - beat * kBeatPeriod, isRight ? 1320.0 : 880.0);
    return isRight ? StereoSample{0, value} : StereoSample{value, 0};
}

SyncedRenderer::SyncedRenderer(const RendererConfiguration& configuration) : configuration_(configuration) {}

void SyncedRenderer::setClockMap(const ClockMap& map) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    shared_.clockMap = map;
    shared_.hasClockMap = true;
}

void SyncedRenderer::clearClockMap() {
    std::lock_guard<std::mutex> lock(controlMutex_);
    shared_.hasClockMap = false;
}

void SyncedRenderer::setPresentationDelay(int64_t nanoseconds) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    shared_.presentationDelay = nanoseconds;
}

void SyncedRenderer::setPlayback(const PlaybackState& playback, int generation, RenderSourceKind kind, StreamBuffer* stream) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    shared_.playback = playback;
    shared_.generation = generation;
    shared_.kind = (kind == RenderSourceKind::stream && stream == nullptr) ? RenderSourceKind::silence : kind;
    shared_.stream = stream;
}

void SyncedRenderer::setChannelMix(const ChannelMix& mix, int64_t fromLeaderTime) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    // If an earlier switch is still pending it is skipped; switches are seconds apart.
    shared_.mixBefore = shared_.mix;
    shared_.mix = mix;
    shared_.mixFromLeaderTime = fromLeaderTime;
}

RenderStats SyncedRenderer::latestStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return published_;
}

void SyncedRenderer::publishStats() {
    if (statsMutex_.try_lock()) {
        published_ = stats_;
        statsMutex_.unlock();
    }
}

void SyncedRenderer::render(bool hasHostTime, int64_t hostTime, int frameCount, double outputRate, float* left, float* right) {
    if (controlMutex_.try_lock()) {
        current_ = shared_;
        controlMutex_.unlock();
    }

    double sourceRate = 0;
    switch (current_.kind) {
    case RenderSourceKind::silence:
        break;
    case RenderSourceKind::metronome:
    case RenderSourceKind::channelTest:
        sourceRate = outputRate;
        break;
    case RenderSourceKind::stream:
        sourceRate = current_.stream ? current_.stream->sampleRate() : 0;
        break;
    }

    stats_.hostTimeValid = hasHostTime;
    if (!hasHostTime || !current_.hasClockMap || !current_.playback.isPlaying() || sourceRate <= 0) {
        std::fill(left, left + frameCount, 0.0f);
        std::fill(right, right + frameCount, 0.0f);
        wasAudible_ = false;
        // Nothing to ramp from while silent.
        appliedMix_ = current_.mix;
        stats_.isPlaying = false;
        stats_.correctionPPM = 0;
        stats_.bufferedSeconds = 0;
        stats_.outputPeak = 0;
        publishStats();
        return;
    }

    // Which instant of the source must be heard when this buffer's first frame is?
    const int64_t audibleAt = hostTime + current_.presentationDelay;
    const int64_t audibleAtLeader = current_.clockMap.leaderTimeFromLocal(audibleAt);
    const int64_t mediaTime = current_.playback.mediaTimeAtLeaderTime(audibleAtLeader);
    const double target = static_cast<double>(mediaTime) * sourceRate / 1e9;
    const double error = target - position_;

    double correction = 0;
    if (current_.generation != lastGeneration_ || std::abs(error) > configuration_.jumpThreshold * sourceRate) {
        if (wasAudible_) {
            fadeRemaining_ = static_cast<int>(configuration_.fadeDuration * outputRate);
        }
        if (current_.generation == lastGeneration_) {
            stats_.jumps += 1;
        }
        lastGeneration_ = current_.generation;
        position_ = target;
        inStreamGap_ = false;
    } else {
        const double limit = configuration_.maximumCorrection;
        const double errorSeconds = error / sourceRate;
        const double timeConstant = configuration_.correctionTimeConstant;
        const double elapsed = frameCount / outputRate;
        integral_ += errorSeconds / (4 * timeConstant * timeConstant) * elapsed;
        integral_ = std::clamp(integral_, -limit, limit);
        correction = std::clamp(errorSeconds / timeConstant + integral_, -limit, limit);
    }

    const double increment = sourceRate / outputRate * (1 + correction);
    switch (current_.kind) {
    case RenderSourceKind::silence:
        std::fill(left, left + frameCount, 0.0f);
        std::fill(right, right + frameCount, 0.0f);
        break;
    case RenderSourceKind::metronome:
        for (int frame = 0; frame < frameCount; ++frame) {
            const float value = metronomeSample((position_ + frame * increment) / outputRate);
            left[frame] = value;
            right[frame] = value;
        }
        break;
    case RenderSourceKind::channelTest:
        for (int frame = 0; frame < frameCount; ++frame) {
            const StereoSample sample = channelTestSample((position_ + frame * increment) / outputRate);
            left[frame] = sample.left;
            right[frame] = sample.right;
        }
        break;
    case RenderSourceKind::stream:
        renderStream(*current_.stream, sourceRate, increment, frameCount, outputRate, left, right);
        break;
    }

    if (fadeRemaining_ > 0) {
        const auto fadeLength = static_cast<float>(std::max(static_cast<int>(configuration_.fadeDuration * outputRate), 1));
        const int count = std::min(fadeRemaining_, frameCount);
        for (int frame = 0; frame < count; ++frame) {
            const float gain = 1 - static_cast<float>(fadeRemaining_ - frame) / fadeLength;
            left[frame] *= gain;
            right[frame] *= gain;
        }
        fadeRemaining_ = std::max(0, fadeRemaining_ - frameCount);
    }
    applyMix(audibleAtLeader >= current_.mixFromLeaderTime ? current_.mix : current_.mixBefore, frameCount, left, right);
    position_ += frameCount * increment;
    wasAudible_ = true;

    float peak = 0;
    for (int frame = 0; frame < frameCount; ++frame) {
        peak = std::max(peak, std::max(std::abs(left[frame]), std::abs(right[frame])));
    }
    // Decays to a tenth within about a quarter second at typical buffer sizes.
    stats_.outputPeak = std::max(static_cast<double>(peak), stats_.outputPeak * 0.9);
    stats_.isPlaying = true;
    stats_.errorMicroseconds = error / sourceRate * 1e6;
    stats_.correctionPPM = correction * 1e6;
    stats_.mediaSeconds = position_ / sourceRate;
    publishStats();
}

void SyncedRenderer::applyMix(const ChannelMix& target, int frameCount, float* left, float* right) {
    const ChannelMix from = appliedMix_;
    appliedMix_ = target;
    if (from == target && target.isIdentity()) return;
    // Ramp the coefficients across the cycle when the mix changed, so the switch is click-free.
    const float step = from == target ? 0 : 1.0f / static_cast<float>(std::max(frameCount, 1));
    for (int frame = 0; frame < frameCount; ++frame) {
        const float t = from == target ? 1 : static_cast<float>(frame + 1) * step;
        const float ll = from.leftFromLeft + (target.leftFromLeft - from.leftFromLeft) * t;
        const float lr = from.leftFromRight + (target.leftFromRight - from.leftFromRight) * t;
        const float rl = from.rightFromLeft + (target.rightFromLeft - from.rightFromLeft) * t;
        const float rr = from.rightFromRight + (target.rightFromRight - from.rightFromRight) * t;
        const float l = left[frame], r = right[frame];
        left[frame] = ll * l + lr * r;
        right[frame] = rl * l + rr * r;
    }
}

void SyncedRenderer::renderStream(const StreamBuffer& buffer, double sourceRate, double increment, int frameCount, double outputRate, float* left, float* right) {
    const FrameRange range = buffer.validRange();
    // Interpolation reads up to two frames past the position.
    const double lastReadable = static_cast<double>(range.upperBound) - 3;
    const bool hasData = static_cast<double>(range.lowerBound) <= position_ && position_ <= lastReadable;
    if (hasData && inStreamGap_) {
        // Data is back (or arrived for a late join): fade in rather than click.
        inStreamGap_ = false;
        fadeRemaining_ = static_cast<int>(configuration_.fadeDuration * outputRate);
    } else if (!hasData && position_ >= 0 && !inStreamGap_) {
        inStreamGap_ = true;
    }

    for (int frame = 0; frame < frameCount; ++frame) {
        const StereoSample sample = interpolate(buffer, range, position_ + frame * increment);
        left[frame] = sample.left;
        right[frame] = sample.right;
    }
    // The writer only overwrites frames far behind playback, but if it ever lapped us
    // mid-read, play silence rather than whatever it wrote there.
    const int64_t oldestRead = std::max(range.lowerBound, static_cast<int64_t>(std::floor(position_)) - 1);
    if (!buffer.isStillValid(oldestRead)) {
        std::fill(left, left + frameCount, 0.0f);
        std::fill(right, right + frameCount, 0.0f);
    }

    // Data runs out within this cycle: fade out up to that point instead of cutting.
    const double endPosition = position_ + (frameCount - 1) * increment;
    if (hasData && endPosition > lastReadable) {
        const int cut = std::min(frameCount, std::max(0, static_cast<int>(std::floor((lastReadable - position_) / increment)) + 1));
        const int fadeLength = std::min(cut, static_cast<int>(configuration_.fadeDuration * outputRate));
        for (int frame = cut - fadeLength; frame < cut; ++frame) {
            const float gain = static_cast<float>(cut - frame) / static_cast<float>(fadeLength + 1);
            left[frame] *= gain;
            right[frame] *= gain;
        }
        std::fill(left + cut, left + frameCount, 0.0f);
        std::fill(right + cut, right + frameCount, 0.0f);
        inStreamGap_ = true;
        stats_.gaps += 1;
    }
    stats_.bufferedSeconds = std::max(0.0, static_cast<double>(range.upperBound) - position_) / sourceRate;
}

}  // namespace asyncmusic
