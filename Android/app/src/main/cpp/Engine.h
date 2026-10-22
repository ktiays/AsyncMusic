//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/net.hpp>
#include <asyncmusic/session.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "AudioOutput.h"

namespace asyncmusic::android {

/// The native half of the Android app: the group session and the audio output playing it,
/// kept in step with the role. The rest of what the session needs from the platform (mDNS,
/// settings, keeping the process alive) lives on the Kotlin side, reached through the
/// callbacks.
class Engine {
public:
    struct Callbacks {
        /// The session changed. Called on the session's thread.
        std::function<void()> onChange;
        /// The leader's speaker settings changed; keep them for next time.
        std::function<void(const GroupSession::SpeakerSettings& settings)> onSpeakerSettingsChange;
        /// Looks up where the leader called `name` listens now (mDNS). Blocks for a few seconds
        /// at most; called on the follower's thread.
        std::function<std::optional<net::Address>(const std::string& name)> resolve;
    };

    Engine(DeviceIdentity identity, GroupSession::SpeakerSettings speakers, double manualOffset, Callbacks callbacks);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    GroupSession& session() { return *session_; }

    /// Audio runs only while part of a group.
    void setRole(GroupSession::Role role);
    void follow(const std::string& leader);

    /// Reads a file (see `loadMediaFile`) and selects it for playing. Blocks while the start of
    /// the file is decoded; returns why it could not be loaded, or an empty string.
    std::string loadTrack(const std::string& path, const std::string& name, const uint8_t digest[32]);

    void setManualOffset(double milliseconds) { output_->setManualOffset(milliseconds); }

    /// Everything a screen shows, as JSON: the session's snapshot, the renderer's statistics and
    /// the output's status (see `NativeEngine.kt` for the reading side).
    std::string snapshotJSON() const;
    /// Just what changes continuously: render statistics, the output and the leader's clock.
    std::string liveJSON() const;

private:
    const Callbacks callbacks_;
    std::unique_ptr<GroupSession> session_;
    std::unique_ptr<AudioOutput> output_;
};

}  // namespace asyncmusic::android
