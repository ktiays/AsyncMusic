//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "clock_estimator.hpp"
#include "net.hpp"
#include "playback.hpp"
#include "protocol.hpp"
#include "serial_queue.hpp"
#include "speakers.hpp"

namespace asyncmusic {

/// The mDNS service type leaders advertise their clock under (UDP).
constexpr const char* kClockSyncServiceType = "_asyncmusic._udp";

/// This device's identity in a group: an ID kept across launches (so a leader remembers which
/// device it paired as a speaker) and the name shown to others (UTF-8).
struct DeviceIdentity {
    uint64_t id = 0;
    std::string name;
};

/// The result of one probe round.
struct ClockSyncRound {
    int index = 0;
    /// Follower host time at which the round finished.
    int64_t localTime = 0;
    ClockVerdict verdict = ClockVerdict::noReplies;
    int probesSent = 0;
    int probesReceived = 0;
    /// The fastest probe of the round, if any reply arrived.
    std::optional<ClockProbe> best;
    /// How far `best` lies from the map fitted after this round, in nanoseconds.
    std::optional<int64_t> residual;
    std::optional<ClockMap> map;
    int64_t minimumRoundTrip = 0;
    double residualRMS = 0;
    int acceptedCount = 0;
    int rejectedCount = 0;
    int windowCount = 0;
};

/// Probes one leader in rounds and keeps a `ClockMap` from this device's host clock to the
/// leader's. A port of the Apple app's `ClockSyncFollower`; see there for the reasoning.
///
/// Each round sends a short burst of pings; only the round's fastest exchange is fed to the
/// estimator. After each round the follower says hello (its device ID and name), which tells the
/// leader it is still there and which speaker channel to give it.
///
/// When the leader stops answering, the follower resolves it again (its port may have changed)
/// and reconnects. The estimate is kept across reconnects, since the leader's clock did not
/// change; it is only discarded if the leader reports a new session.
///
/// Callbacks run on the follower's own thread. They must not destroy the follower.
class ClockSyncFollower {
public:
    struct Configuration {
        int probesPerRound = 8;
        int64_t probeSpacing = 15'000'000;
        /// How long to wait for stragglers after the last ping of a round.
        int64_t replyGrace = 250'000'000;
        /// Rounds run faster at first so a usable offset is available within a few seconds.
        int warmupRounds = 10;
        int64_t warmupInterval = 300'000'000;
        int64_t steadyInterval = 1'000'000'000;
        /// Reconnect after this many consecutive rounds without a single reply.
        int silentRoundsBeforeReconnect = 5;
        int64_t reconnectDelay = 1'000'000'000;
    };

    enum class State : uint8_t {
        connecting,
        syncing,
        reconnecting,
        failed,
        stopped,
    };

    /// Finds the leader's current address, e.g. by resolving its mDNS service. Called on the
    /// follower's thread whenever it (re)connects; may block for a few seconds.
    using Resolver = std::function<std::optional<net::Address>()>;

    ClockSyncFollower(Resolver resolver, DeviceIdentity identity, Configuration configuration);
    ClockSyncFollower(Resolver resolver, DeviceIdentity identity) : ClockSyncFollower(std::move(resolver), std::move(identity), Configuration{}) {}
    ~ClockSyncFollower();
    ClockSyncFollower(const ClockSyncFollower&) = delete;
    ClockSyncFollower& operator=(const ClockSyncFollower&) = delete;

    /// Set before `start()`.
    std::function<void(State state, const std::string& reason)> onStateChange;
    std::function<void(const ClockSyncRound& round)> onRound;
    /// The leader's playback state changed (not called for its periodic repeats).
    std::function<void(const PlaybackState& state)> onPlayback;
    /// The leader's audio stream endpoint became known or changed: the leader's address with
    /// the port it announced.
    std::function<void(const net::Address& endpoint)> onAudioEndpoint;
    /// The leader assigned this device a speaker channel (not called for repeats).
    std::function<void(SpeakerLayout layout, SpeakerChannel channel, int64_t switchLeaderTime)> onSpeakers;

    void start();
    /// Stops and waits for the follower's threads.
    void stop();

private:
    struct Connection;

    void connect();
    void reconnect(const std::string& reason);
    void closeConnection();
    void startRound();
    void sendPing(int round, int index);
    void finishRound(int round);
    void sendHello();
    void handle(const ClockSyncMessage& message, int64_t receivedAt, uint64_t connection);

    const Configuration configuration_;
    const Resolver resolver_;
    const DeviceIdentity identity_;

    SerialQueue queue_;
    // Everything below is confined to `queue_`.
    std::unique_ptr<Connection> connection_;
    uint64_t connectionCount_ = 0;
    bool running_ = false;
    int silentRounds_ = 0;
    ClockEstimator estimator_;
    int roundIndex_ = 0;
    std::vector<ClockProbe> roundProbes_;
    int roundProbesSent_ = 0;
    std::optional<uint64_t> leaderSession_;
    std::optional<std::pair<uint64_t, uint32_t>> playbackRevision_;
    std::optional<std::pair<uint64_t, uint32_t>> speakersRevision_;
    int64_t lastSleepDuration_ = 0;
    std::optional<net::Address> audioEndpoint_;
};

}  // namespace asyncmusic
