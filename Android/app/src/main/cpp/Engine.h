//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/session.hpp>
#include <asyncmusic/share_center.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "AudioOutput.h"

namespace asyncmusic::android {

/// The native half of the Android app: the group session, the audio output playing it, and the
/// share center on top that listens for shares, joins them and shares a file, starting and
/// stopping the output with the role. Keeping the process alive and the settings live on the
/// Kotlin side, which learns about changes through the callbacks.
class Engine {
public:
    struct Callbacks {
        /// The share center's state or the session changed. Called on their threads.
        std::function<void()> onChange;
        /// A new share arrived and alerts are on.
        std::function<void(const ShareCenter::Share& share)> onAlert;
        /// The leader's speaker settings changed; keep them for next time.
        std::function<void(const GroupSession::SpeakerSettings& settings)> onSpeakerSettingsChange;
    };

    struct Options {
        DeviceIdentity identity;
        ShareCenter::Device device;
        GroupSession::SpeakerSettings speakers;
        double manualOffset = 0;
        float volume = 1;
        bool alertsEnabled = true;
    };

    Engine(Options options, Callbacks callbacks);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    GroupSession& session() { return *session_; }
    ShareCenter& center() { return *center_; }

    /// Reads a file (see `loadMediaFile`) and selects it for playing. Blocks while the start of
    /// the file is decoded; returns why it could not be loaded, or an empty string.
    std::string loadTrack(const std::string& path, const std::string& name, const uint8_t digest[32]);
    /// While the app copies and loads the source, and why that failed (empty: it didn't).
    void setSourceStatus(bool isLoading, const std::string& error);

    void setManualOffset(double milliseconds) { output_->setManualOffset(milliseconds); }
    void setVolume(float volume) { output_->setVolume(volume); }
    /// Listening: silences this device only, keeping the stream and the clock sync.
    void setPausedHere(bool paused) { output_->setMuted(paused); }

    /// Sharing: plays the selected file from the top for everyone, or stops it.
    void playSelected();
    void stopPlayback() { session_->stopPlayback(); }

    /// The share center's state as JSON (see `ShareState` in `Models.kt`).
    std::string stateJSON() const;
    /// Everything diagnostics shows, as JSON: the session's snapshot, the renderer's statistics
    /// and the output's status (see `NativeEngine.kt` for the reading side).
    std::string snapshotJSON() const;
    /// Just what changes continuously: render statistics, the output and the leader's clock.
    std::string liveJSON() const;

private:
    /// Changes the session's role for the share center, with the output running while in a group.
    void setRole(GroupSession::Role role);

    const Callbacks callbacks_;
    std::unique_ptr<GroupSession> session_;
    std::unique_ptr<AudioOutput> output_;
    std::unique_ptr<ShareCenter> center_;

    std::atomic<bool> isLoadingSource_{false};
    mutable std::mutex sourceMutex_;
    std::string sourceError_;
};

/// A share as JSON, as `stateJSON` writes each one (also handed over with alerts).
std::string shareJSON(const ShareCenter::Share& share);

}  // namespace asyncmusic::android
