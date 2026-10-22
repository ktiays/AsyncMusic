//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/renderer.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct AAudioStreamStruct;

namespace asyncmusic::android {

/// Plays a `SyncedRenderer` on the current output device through AAudio (float stereo at the
/// device's own rate, data callback), stamping every buffer with the host time at which its
/// first frame is heard.
///
/// `AAudioStream_getTimestamp` pairs a frame position with the `CLOCK_MONOTONIC` time at which
/// that frame is presented (the host clock on Android). It is not meant for the real-time
/// callback, so a control thread polls it, fits a line through the last few seconds of
/// readings (frames against time, which follows the DAC's actual rate without the readings'
/// scatter) and publishes the line; the callback evaluates it for the buffer's first frame.
/// Only the manual offset remains as the renderer's presentation delay.
///
/// When the device goes away (headphones unplugged, Bluetooth switched), AAudio disconnects
/// the stream and the control thread opens a new one on whatever the output is now.
class AudioOutput {
public:
    struct Status {
        bool isRunning = false;
        std::string error;
        /// The AAudio device ID (the app maps it to a name with `AudioManager`).
        int deviceID = 0;
        double sampleRate = 0;
        int channels = 0;
        /// The stream's buffer and its burst (the callback size), in seconds.
        double bufferDuration = 0;
        double burstDuration = 0;
        /// From writing a frame to hearing it, as the timestamps say, in seconds.
        double latency = 0;
        /// How far the raw timestamps scatter around the fitted timeline (RMS), in microseconds.
        double timestampJitter = 0;
        bool lowLatency = false;
        int underruns = 0;
    };

    explicit AudioOutput(SyncedRenderer& renderer);
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    void start();
    void stop();

    /// Extra delay in milliseconds, for when this device sounds early or late compared with the
    /// others (Bluetooth latency in particular is not always reported correctly).
    void setManualOffset(double milliseconds);
    double manualOffset() const { return manualOffset_.load(); }

    Status status() const;

private:
    /// The fitted line `time = baseTime + slope * (frame - baseFrame)`.
    struct Line {
        double baseFrame = 0;
        double baseTime = 0;
        double slope = 0;
    };

    void run();
    /// Plays until stopped or the stream disconnects; returns false on a failure worth reporting.
    bool play();
    int32_t render(float* interleaved, int32_t frameCount);
    void publish(const Line& line);
    bool readLine(Line& line) const;
    void setStatus(const Status& status);

    static int32_t dataCallback(AAudioStreamStruct* stream, void* user, void* audio, int32_t frameCount);
    static void errorCallback(AAudioStreamStruct* stream, void* user, int32_t error);

    SyncedRenderer& renderer_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> disconnected_{false};
    std::atomic<double> manualOffset_{0};
    std::mutex wakeMutex_;
    std::condition_variable wake_;

    // The line, published by the control thread to the render thread through a sequence lock,
    // so the render thread never waits. An odd sequence means a write is under way.
    std::atomic<uint32_t> lineSequence_{0};
    std::atomic<bool> hasLine_{false};
    std::atomic<double> lineBaseFrame_{0};
    std::atomic<double> lineBaseTime_{0};
    std::atomic<double> lineSlope_{0};

    // Render-thread state, reset (with the stream stopped) for every new stream.
    static constexpr int kChunkFrames = 2048;
    double sampleRate_ = 0;
    int64_t framesRendered_ = 0;
    float left_[kChunkFrames] = {};
    float right_[kChunkFrames] = {};

    mutable std::mutex mutex_;
    Status status_;
};

}  // namespace asyncmusic::android
