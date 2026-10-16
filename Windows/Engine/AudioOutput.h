//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/renderer.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace asyncmusic::windows {

/// Plays a `SyncedRenderer` on the default output device through WASAPI (shared mode,
/// event-driven), stamping every buffer with the host time at which its first frame is heard.
///
/// That time comes from `IAudioClock::GetPosition`, which pairs the device's play position with
/// the performance counter (the host clock on Windows): the next buffer's first frame is heard
/// after the frames still queued ahead of it have played. Only the manual offset remains as the
/// renderer's presentation delay.
///
/// Those readings scatter by a few hundred microseconds from one callback to the next, while
/// the device really plays at a steady rate. Each buffer is therefore stamped from a line fitted
/// through the last few seconds of readings (frames written against time), which follows the
/// DAC's actual rate and drift without the scatter.
///
/// Follows the default device: when it changes or goes away, the stream is reopened on the new
/// one.
class AudioOutput {
public:
    struct Status {
        bool isRunning = false;
        std::string error;
        std::string deviceName;
        double sampleRate = 0;
        int channels = 0;
        /// The shared-mode buffer, in seconds.
        double bufferDuration = 0;
        /// Reported by the stream; WASAPI timestamps already include it.
        double streamLatency = 0;
        /// How far the device's raw timestamps scatter around the fitted timeline (RMS), in
        /// microseconds. Only the fitted timeline is used for playback.
        double timestampJitter = 0;
    };

    explicit AudioOutput(SyncedRenderer& renderer);
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    void start();
    void stop();

    /// Extra delay in milliseconds, for when this device sounds early or late compared with the
    /// others (the device's own latency is not always reported, e.g. over Bluetooth).
    void setManualOffset(double milliseconds);
    double manualOffset() const { return manualOffset_.load(); }

    Status status() const;

private:
    class DeviceNotifications;

    void run();
    /// Plays until stopped or the device goes away; returns false on a failure worth reporting.
    bool play();
    void setStatus(const Status& status);

    SyncedRenderer& renderer_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> deviceChanged_{false};
    std::atomic<double> manualOffset_{0};
    void* wake_ = nullptr;
    mutable std::mutex mutex_;
    Status status_;
};

}  // namespace asyncmusic::windows
