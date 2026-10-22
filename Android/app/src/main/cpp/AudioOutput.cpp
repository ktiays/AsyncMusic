//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "AudioOutput.h"

#include <aaudio/AAudio.h>

#include <asyncmusic/host_clock.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace asyncmusic::android {

namespace {

constexpr int kChannels = 2;
/// How often the control thread reads the stream's timestamp.
constexpr auto kTimestampInterval = std::chrono::milliseconds(50);

std::string describe(aaudio_result_t result) {
    return std::string(AAudio_convertResultToText(result)) + " (" + std::to_string(result) + ")";
}

/// Fits `time = intercept + slope * frame` through the most recent timestamp readings.
class TimelineFit {
public:
    void reset() {
        count_ = 0;
        next_ = 0;
    }

    /// Adds a reading. Returns false if there is no line yet.
    bool add(double frame, double time, double nominalSlope) {
        if (count_ > 0) {
            // A reading far off the line means the stream skipped (an underrun, a reconfigured
            // route): the old readings no longer describe it.
            if (std::abs(time - fittedTime(frame)) > kDiscontinuity) reset();
            // The same reading again tells nothing new.
            else if (frame == frames_[(next_ + kCapacity - 1) % kCapacity]) return true;
        }
        frames_[next_] = frame;
        times_[next_] = time;
        next_ = (next_ + 1) % kCapacity;
        if (count_ < kCapacity) ++count_;
        refit(frame, time, nominalSlope);
        return true;
    }

    double fittedTime(double frame) const { return baseTime_ + intercept_ + slope_ * (frame - baseFrame_); }
    double slope() const { return slope_; }
    double jitter() const { return jitter_; }

private:
    static constexpr int kCapacity = 80;  // 4 s of readings
    static constexpr int kMinimumCount = 10;
    static constexpr double kDiscontinuity = 3e6;  // ns

    void refit(double baseFrame, double baseTime, double nominalSlope) {
        // Relative to the newest reading, so the sums stay small and precise.
        baseFrame_ = baseFrame;
        baseTime_ = baseTime;
        double sumX = 0, sumY = 0, sumXX = 0, sumXY = 0;
        for (int i = 0; i < count_; ++i) {
            const double x = frames_[i] - baseFrame;
            const double y = times_[i] - baseTime;
            sumX += x;
            sumY += y;
            sumXX += x * x;
            sumXY += x * y;
        }
        const double n = count_;
        const double denominator = n * sumXX - sumX * sumX;
        if (count_ < kMinimumCount || denominator <= 0) {
            // Until there are enough readings, go through the newest at the nominal rate.
            slope_ = nominalSlope;
            intercept_ = 0;
            jitter_ = 0;
            return;
        }
        slope_ = (n * sumXY - sumX * sumY) / denominator;
        intercept_ = (sumY - slope_ * sumX) / n;
        double squares = 0;
        for (int i = 0; i < count_; ++i) {
            const double residual = (times_[i] - baseTime) - (intercept_ + slope_ * (frames_[i] - baseFrame));
            squares += residual * residual;
        }
        jitter_ = std::sqrt(squares / n) / 1e3;
    }

    double frames_[kCapacity] = {};
    double times_[kCapacity] = {};
    int count_ = 0;
    int next_ = 0;
    double baseFrame_ = 0;
    double baseTime_ = 0;
    double slope_ = 0;
    double intercept_ = 0;
    double jitter_ = 0;
};

}  // namespace

AudioOutput::AudioOutput(SyncedRenderer& renderer) : renderer_(renderer) {}

AudioOutput::~AudioOutput() {
    stop();
}

void AudioOutput::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void AudioOutput::stop() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.isRunning = false;
}

void AudioOutput::setManualOffset(double milliseconds) {
    manualOffset_ = milliseconds;
    renderer_.setPresentationDelay(static_cast<int64_t>(milliseconds * 1e6));
}

AudioOutput::Status AudioOutput::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void AudioOutput::setStatus(const Status& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = status;
}

void AudioOutput::run() {
    while (!stopping_) {
        disconnected_ = false;
        const bool reopenRightAway = play();
        if (stopping_) break;
        // After a failure (no device, device busy) try again a little later.
        if (!reopenRightAway) {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_.load(); });
        }
    }
}

bool AudioOutput::play() {
    Status status;
    const auto fail = [&](const std::string& message) {
        status.isRunning = false;
        status.error = message;
        setStatus(status);
        return false;
    };

    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) return fail(describe(result));
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(builder, kChannels);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    // The schedule does not need a short buffer (every buffer is stamped with when it is heard),
    // and the regular path keeps the CPU and the radio calmer.
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_NONE);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_MEDIA);
    AAudioStreamBuilder_setContentType(builder, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(builder, &AudioOutput::dataCallback, this);
    AAudioStreamBuilder_setErrorCallback(builder, &AudioOutput::errorCallback, this);
    AAudioStream* stream = nullptr;
    result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (result != AAUDIO_OK) return fail("Could not open the audio output: " + describe(result));

    sampleRate_ = AAudioStream_getSampleRate(stream);
    framesRendered_ = 0;
    hasLine_ = false;
    status.deviceID = AAudioStream_getDeviceId(stream);
    status.sampleRate = sampleRate_;
    status.channels = AAudioStream_getChannelCount(stream);
    status.lowLatency = AAudioStream_getPerformanceMode(stream) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY;
    if (sampleRate_ > 0) {
        status.bufferDuration = AAudioStream_getBufferSizeInFrames(stream) / sampleRate_;
        status.burstDuration = AAudioStream_getFramesPerBurst(stream) / sampleRate_;
    }
    if (status.channels != kChannels || !(sampleRate_ > 0)) {
        AAudioStream_close(stream);
        return fail("Unsupported audio output format");
    }

    result = AAudioStream_requestStart(stream);
    if (result != AAUDIO_OK) {
        AAudioStream_close(stream);
        return fail("Could not start the audio output: " + describe(result));
    }
    status.isRunning = true;
    setStatus(status);

    TimelineFit timeline;
    const double nominalSlope = 1e9 / sampleRate_;
    int readings = 0;
    while (!stopping_ && !disconnected_) {
        {
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, kTimestampInterval, [this] { return stopping_.load() || disconnected_.load(); });
        }
        if (stopping_ || disconnected_) break;
        if (AAudioStream_getState(stream) == AAUDIO_STREAM_STATE_DISCONNECTED) break;

        int64_t framePosition = 0;
        int64_t time = 0;
        if (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &framePosition, &time) == AAUDIO_OK && time > 0) {
            timeline.add(static_cast<double>(framePosition), static_cast<double>(time), nominalSlope);
            publish(Line{static_cast<double>(framePosition), timeline.fittedTime(static_cast<double>(framePosition)), timeline.slope()});
        }
        if (++readings % 10 == 0) {
            const int64_t written = AAudioStream_getFramesWritten(stream);
            std::lock_guard<std::mutex> lock(mutex_);
            status_.timestampJitter = timeline.jitter();
            status_.latency = hasLine_ ? (timeline.fittedTime(static_cast<double>(written)) - static_cast<double>(hostClockNanoseconds())) / 1e9 : 0;
            status_.underruns = AAudioStream_getXRunCount(stream);
            status_.bufferDuration = AAudioStream_getBufferSizeInFrames(stream) / sampleRate_;
        }
    }
    // Closing waits for a callback in progress; none runs after it.
    AAudioStream_requestStop(stream);
    AAudioStream_close(stream);
    hasLine_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.isRunning = false;
    }
    return true;
}

void AudioOutput::publish(const Line& line) {
    const uint32_t sequence = lineSequence_.load(std::memory_order_relaxed);
    lineSequence_.store(sequence + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    lineBaseFrame_.store(line.baseFrame, std::memory_order_relaxed);
    lineBaseTime_.store(line.baseTime, std::memory_order_relaxed);
    lineSlope_.store(line.slope, std::memory_order_relaxed);
    lineSequence_.store(sequence + 2, std::memory_order_release);
    hasLine_.store(true, std::memory_order_release);
}

bool AudioOutput::readLine(Line& line) const {
    if (!hasLine_.load(std::memory_order_acquire)) return false;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t before = lineSequence_.load(std::memory_order_acquire);
        if (before & 1) continue;
        line.baseFrame = lineBaseFrame_.load(std::memory_order_relaxed);
        line.baseTime = lineBaseTime_.load(std::memory_order_relaxed);
        line.slope = lineSlope_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (lineSequence_.load(std::memory_order_relaxed) == before) return true;
    }
    return false;
}

int32_t AudioOutput::render(float* interleaved, int32_t frameCount) {
    Line line;
    const bool hasLine = readLine(line);
    for (int32_t offset = 0; offset < frameCount;) {
        const int count = std::min<int>(kChunkFrames, frameCount - offset);
        const double frame = static_cast<double>(framesRendered_ + offset);
        const int64_t hostTime = hasLine ? static_cast<int64_t>(std::llround(line.baseTime + line.slope * (frame - line.baseFrame))) : 0;
        renderer_.render(hasLine, hostTime, count, sampleRate_, left_, right_);
        float* out = interleaved + 2 * static_cast<size_t>(offset);
        for (int i = 0; i < count; ++i) {
            out[2 * i] = left_[i];
            out[2 * i + 1] = right_[i];
        }
        offset += count;
    }
    framesRendered_ += frameCount;
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

int32_t AudioOutput::dataCallback(AAudioStreamStruct*, void* user, void* audio, int32_t frameCount) {
    return static_cast<AudioOutput*>(user)->render(static_cast<float*>(audio), frameCount);
}

void AudioOutput::errorCallback(AAudioStreamStruct*, void* user, int32_t error) {
    auto* output = static_cast<AudioOutput*>(user);
    // Anything but a disconnect leaves the stream usable. The stream may only be closed from
    // another thread, so wake the control thread to do it.
    if (error != AAUDIO_ERROR_DISCONNECTED) return;
    {
        std::lock_guard<std::mutex> lock(output->wakeMutex_);
        output->disconnected_ = true;
    }
    output->wake_.notify_all();
}

}  // namespace asyncmusic::android
