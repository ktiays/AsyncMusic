//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "audio_stream.hpp"
#include "clock_sync.hpp"
#include "player.hpp"
#include "serial_queue.hpp"
#include "speakers.hpp"

namespace asyncmusic {

/// Drives clock sync and synced playback for a UI: follows a leader (and, later, leads), and
/// keeps a short history of rounds and a log for diagnostics. The C++ counterpart of the Apple
/// app's `ClockSyncSession`.
///
/// The platform supplies what is not portable: the audio output (which renders from
/// `player().renderer()`), finding leaders (mDNS), and the device's identity.
///
/// All state lives on the session's own thread. The UI reads it through `snapshot()`, from any
/// thread, and learns about changes from `onChange`, which is called on the session's thread
/// (hop to the UI thread before touching UI from it).
class GroupSession {
public:
    enum class Role : uint8_t {
        off,
        leader,
        follower,
    };

    struct LogLine {
        std::chrono::system_clock::time_point date;
        std::string text;
    };

    struct Snapshot {
        Role role = Role::off;
        std::string status = "Idle";
        DeviceIdentity identity;

        // Follower
        /// The name of the leader being followed, if any.
        std::optional<std::string> connectedLeader;
        std::optional<ClockSyncFollower::State> followerState;
        bool isAudioConnected = false;
        std::optional<ClockSyncRound> latestRound;
        std::vector<ClockSyncRound> rounds;
        /// The layout the leader announced; this device's channel is `channel`.
        SpeakerLayout remoteLayout = SpeakerLayout::everywhere;

        // Playback
        PlaybackState playback;
        SpeakerChannel channel = SpeakerChannel::full;
        std::optional<ClockMap> clockMap;
        bool isWaitingForAudio = false;

        std::vector<LogLine> log;
        /// Increases with every change.
        uint64_t revision = 0;
    };

    explicit GroupSession(DeviceIdentity identity);
    ~GroupSession();
    GroupSession(const GroupSession&) = delete;
    GroupSession& operator=(const GroupSession&) = delete;

    /// Set before using the session.
    std::function<void()> onChange;

    /// The renderer the audio output plays from. `render` may be called from the audio thread
    /// at any time; set the output's presentation delay on it directly.
    SyncedRenderer& renderer() { return player_.renderer(); }

    /// Switches role. Leading is not available in this port yet.
    void setRole(Role role);
    /// Changes how this device introduces itself; takes effect from the next `follow`.
    void setIdentity(DeviceIdentity identity);
    /// Follows the leader called `name`, found by `resolver` (an mDNS lookup, typically).
    void follow(const std::string& name, ClockSyncFollower::Resolver resolver);
    void clearLog();

    Snapshot snapshot() const;
    /// A short description of `source`, e.g. "metronome" or "streamed file 1a2b…".
    static std::string describe(uint64_t source);

private:
    void tearDown();
    void connectAudioStream(const net::Address& endpoint);
    void followerStateChanged(ClockSyncFollower::State state, const std::string& reason);
    void record(const ClockSyncRound& round);
    void applyRemotePlayback(const PlaybackState& state);
    void applyRemoteSpeakers(SpeakerLayout layout, SpeakerChannel channel, int64_t switchTime);
    void resetRemoteSpeakers();
    void tick();
    void append(const std::string& text);
    /// Copies player state into the snapshot and notifies. Call after every change.
    void publish();

    SerialQueue queue_;
    // Confined to `queue_`.
    Player player_;
    std::unique_ptr<ClockSyncFollower> follower_;
    std::unique_ptr<AudioStreamClient> audioClient_;
    /// Bumped whenever the follower or audio client is replaced, so late callbacks from an old
    /// one are ignored.
    uint64_t generation_ = 0;
    /// The same for the audio client alone, which is replaced when the stream's port changes.
    uint64_t audioGeneration_ = 0;
    bool running_ = true;

    mutable std::mutex mutex_;
    Snapshot state_;
};

}  // namespace asyncmusic
