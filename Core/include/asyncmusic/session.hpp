//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "audio_stream.hpp"
#include "clock_sync.hpp"
#include "media.hpp"
#include "player.hpp"
#include "serial_queue.hpp"
#include "speakers.hpp"

namespace asyncmusic {

/// Drives clock sync and synced playback for a UI: either leads (serves its clock, decides what
/// plays and streams it) or follows a leader, and keeps a short history of rounds and a log for
/// diagnostics. The C++ counterpart of the Apple app's `ClockSyncSession`.
///
/// The leader also decides how the music is spread over the group (`SpeakerLayout`): every
/// device playing the full mix, or a stereo pair with one device per channel. Channels are
/// assigned to devices by their stable ID and remembered, so the same two devices pair up again
/// next time. While a channel has no device present, everyone plays the full mix.
///
/// The platform supplies what is not portable: the audio output (which renders from
/// `renderer()`), mDNS (finding leaders, and advertising `Snapshot::leaderPort`), decoding
/// files (`MediaFile`), the device's identity and keeping the speaker settings.
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

    /// What the leader remembers about its speakers across launches.
    struct SpeakerSettings {
        SpeakerLayout layout = SpeakerLayout::everywhere;
        /// The device chosen for each channel of a layout. Kept per channel across layouts;
        /// entries for devices not around are kept too, in case they come back.
        std::map<SpeakerChannel, uint64_t> assignments;
    };

    struct Track {
        uint64_t id = 0;
        std::string name;
        double duration = 0;
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

        // Leader
        /// The UDP port to advertise under `kClockSyncServiceType` (0 while not listening).
        uint16_t leaderPort = 0;
        int followerCount = 0;
        /// Followers that introduced themselves, with whether each is reachable now.
        std::vector<ClockSyncLeader::Member> members;
        SpeakerSettings speakers;
        /// The channel each device plays right now (absent: the full mix), as sent to the group.
        std::map<uint64_t, SpeakerChannel> activeChannels;
        /// Whether every channel of the layout has a device that is present.
        bool isLayoutComplete = false;
        /// True while a file is being buffered on followers before it starts.
        bool isPreparingPlayback = false;
        /// The file "Play" plays.
        std::optional<Track> selectedTrack;

        // Playback
        PlaybackState playback;
        SpeakerChannel channel = SpeakerChannel::full;
        std::optional<ClockMap> clockMap;
        bool isWaitingForAudio = false;

        std::vector<LogLine> log;
        /// Increases with every change.
        uint64_t revision = 0;

        /// This device, or a follower that is reachable now.
        bool isDevicePresent(uint64_t id) const;
        /// The name of this device or of a member.
        std::optional<std::string> nameOfDevice(uint64_t id) const;
    };

    /// How far ahead the leader schedules the start of a generated source. Must comfortably
    /// exceed the time for the command to reach followers plus the largest output latency in
    /// the group (Bluetooth can be 200+ ms). Followers that hear about a start late join mid-way.
    static constexpr int64_t kStartLeadTime = 1'000'000'000;
    /// Files are first streamed to every follower; playback starts once each has
    /// `kStartBufferDuration` buffered (adaptive start), but waits at most this long.
    static constexpr double kMaximumStartWait = 3.0;
    static constexpr double kStartBufferDuration = 0.5;
    /// Once everyone is ready, the start is scheduled this far ahead, covering the play command's
    /// trip to followers and their output latency.
    static constexpr int64_t kStartMargin = 500'000'000;
    /// Channel changes take effect this far ahead, so every device switches at the same moment.
    static constexpr int64_t kChannelSwitchLead = 400'000'000;

    explicit GroupSession(DeviceIdentity identity);
    GroupSession(DeviceIdentity identity, SpeakerSettings speakers);
    ~GroupSession();
    GroupSession(const GroupSession&) = delete;
    GroupSession& operator=(const GroupSession&) = delete;

    /// Set before using the session.
    std::function<void()> onChange;
    /// The leader changed its speaker layout or assignments; keep them for next time. Called on
    /// the session's thread.
    std::function<void(const SpeakerSettings& settings)> onSpeakerSettingsChange;

    /// The renderer the audio output plays from. `render` may be called from the audio thread
    /// at any time; set the output's presentation delay on it directly.
    SyncedRenderer& renderer() { return player_.renderer(); }

    void setRole(Role role);
    /// Changes how this device introduces itself; takes effect from the next `follow`.
    void setIdentity(DeviceIdentity identity);
    /// Follows the leader called `name`, found by `resolver` (an mDNS lookup, typically).
    void follow(const std::string& name, ClockSyncFollower::Resolver resolver);
    void clearLog();

    // Leader

    /// Makes a file available to play and selects it.
    void addTrack(std::shared_ptr<const MediaFile> file);
    /// Starts `source` from the top on every device. The metronome and the channel test start
    /// after `kStartLeadTime`; files as soon as every follower has buffered enough.
    void play(uint64_t source);
    void stopPlayback();
    void setSpeakerLayout(SpeakerLayout layout);
    /// Plays `channel` on `device` (none: nobody). A device already playing another channel
    /// trades places with the one it replaces.
    void assign(std::optional<uint64_t> device, SpeakerChannel channel);
    /// Swaps the devices of a two-channel layout (left and right of a stereo pair).
    void swapChannels();

    Snapshot snapshot() const;
    /// A short description of `source`, e.g. "metronome", "“Song”" or "streamed file 1a2b…".
    std::string describe(uint64_t source) const;

private:
    void tearDown();
    void startLeading();
    void leaderStateChanged(ClockSyncLeader::State state, const std::string& reason);
    void scheduleEndOfTrack(const PlaybackState& state);
    void setAssignments(const std::map<SpeakerChannel, uint64_t>& assignments);
    void storeSpeakers(const SpeakerSettings& settings);
    void fillOpenChannels();
    void updateSpeakers();
    /// Runs `work` on the session's thread unless the session moved on (role change, new
    /// leader) meanwhile.
    std::function<void(std::function<void()>)> onSession();
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
    std::unique_ptr<ClockSyncLeader> leader_;
    std::unique_ptr<AudioStreamServer> audioServer_;
    /// The stream the leader is sending, as reported by the player.
    std::optional<OutgoingStream> outgoingStream_;
    /// Identifies the play request whose adaptive start is pending (0: none), so a newer request
    /// or a stop cancels it.
    uint64_t pendingStart_ = 0;
    uint64_t playRequests_ = 0;
    uint64_t endOfTrack_ = 0;
    /// The layout last sent to followers, so a layout change is announced even if no device's
    /// channel changed (e.g. a stereo pair still waiting for its second device).
    std::optional<SpeakerLayout> announcedLayout_;
    /// Bumped whenever the follower or audio client is replaced, so late callbacks from an old
    /// one are ignored.
    uint64_t generation_ = 0;
    /// The same for the audio client alone, which is replaced when the stream's port changes.
    uint64_t audioGeneration_ = 0;
    bool running_ = true;

    mutable std::mutex mutex_;
    Snapshot state_;
    /// Names of the files added, for `describe`.
    std::map<uint64_t, std::string> trackNames_;
};

}  // namespace asyncmusic
