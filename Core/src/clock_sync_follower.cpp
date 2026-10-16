//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include <algorithm>

#include "asyncmusic/clock_sync.hpp"
#include "asyncmusic/host_clock.hpp"
#include "asyncmusic/protocol.hpp"
#include "text.hpp"

namespace asyncmusic {

struct ClockSyncFollower::Connection {
    uint64_t id = 0;
    net::Address address;
    net::UDPSocket socket;
    std::atomic<bool> stop{false};
    std::thread receiver;
};

ClockSyncFollower::ClockSyncFollower(Resolver resolver, DeviceIdentity identity, Configuration configuration)
    : configuration_(configuration), resolver_(std::move(resolver)), identity_(std::move(identity)), queue_("asyncmusic.clock-sync.follower") {}

ClockSyncFollower::~ClockSyncFollower() {
    stop();
    queue_.shutdown();
}

void ClockSyncFollower::start() {
    queue_.async([this] {
        if (running_) return;
        running_ = true;
        lastSleepDuration_ = sleptNanoseconds();
        if (onStateChange) onStateChange(State::connecting, "");
        connect();
        startRound();
    });
}

void ClockSyncFollower::stop() {
    queue_.sync([this] {
        if (!running_) return;
        running_ = false;
        closeConnection();
        if (onStateChange) onStateChange(State::stopped, "");
    });
}

void ClockSyncFollower::connect() {
    if (!running_ || connection_) return;
    const std::optional<net::Address> address = resolver_ ? resolver_() : std::nullopt;
    if (!address) {
        reconnect("Leader not found");
        return;
    }
    auto connection = std::make_unique<Connection>();
    connection->id = ++connectionCount_;
    connection->address = *address;
    if (!connection->socket.open(address->family()) || !connection->socket.connect(*address)) {
        reconnect(net::lastErrorDescription());
        return;
    }
    connection->socket.setVoiceTrafficClass();

    Connection* raw = connection.get();
    connection->receiver = std::thread([this, raw] {
        uint8_t buffer[512];
        while (!raw->stop.load(std::memory_order_relaxed)) {
            const int size = raw->socket.receive(buffer, sizeof(buffer), 100);
            // Stamp before doing anything else, mirroring the leader's t1.
            const int64_t receivedAt = hostClockNanoseconds();
            ClockSyncMessage message;
            if (size <= 0 || !ClockSyncMessage::decode(buffer, static_cast<size_t>(size), message)) continue;
            const uint64_t id = raw->id;
            queue_.async([this, message = std::move(message), receivedAt, id] { handle(message, receivedAt, id); });
        }
    });
    connection_ = std::move(connection);
    silentRounds_ = 0;
    if (onStateChange) onStateChange(State::syncing, "");
    sendHello();
}

void ClockSyncFollower::closeConnection() {
    if (!connection_) return;
    connection_->stop = true;
    if (connection_->receiver.joinable()) connection_->receiver.join();
    connection_.reset();
}

void ClockSyncFollower::reconnect(const std::string& reason) {
    if (!running_) return;
    if (onStateChange) onStateChange(State::reconnecting, reason);
    closeConnection();
    const uint64_t attempt = ++connectionCount_;
    queue_.asyncAfter(configuration_.reconnectDelay, [this, attempt] {
        // A later reconnect supersedes this one.
        if (connectionCount_ == attempt) connect();
    });
}

void ClockSyncFollower::startRound() {
    if (!running_) return;
    // If this device slept, its host clock stood still while the leader's ran on, so the whole
    // estimate is off by the sleep time. Start over (and assume the connection died).
    const int64_t slept = sleptNanoseconds();
    if (slept - lastSleepDuration_ > kSleepDetectionThreshold) {
        lastSleepDuration_ = slept;
        estimator_.reset();
        reconnect("Device slept");
    }
    ++roundIndex_;
    roundProbes_.clear();
    roundProbesSent_ = 0;
    const int round = roundIndex_;
    for (int index = 0; index < configuration_.probesPerRound; ++index) {
        queue_.asyncAfter(configuration_.probeSpacing * index, [this, round, index] { sendPing(round, index); });
    }
    const int64_t roundLength = configuration_.probeSpacing * (configuration_.probesPerRound - 1);
    queue_.asyncAfter(roundLength + configuration_.replyGrace, [this, round] { finishRound(round); });
}

void ClockSyncFollower::sendPing(int round, int index) {
    if (!running_ || roundIndex_ != round || !connection_) return;
    const uint32_t sequence = static_cast<uint32_t>(round) << 8 | static_cast<uint32_t>(index);
    // Stamp right before handing the datagram to the network stack.
    const Bytes ping = ClockSyncMessage::makePing(sequence, hostClockNanoseconds()).encode();
    connection_->socket.send(ping.data(), ping.size());
    ++roundProbesSent_;
}

void ClockSyncFollower::finishRound(int round) {
    if (!running_ || roundIndex_ != round) return;
    const ClockVerdict verdict = estimator_.addRound(roundProbes_.data(), roundProbes_.size());
    if (connection_ && roundProbesSent_ > 0 && roundProbes_.empty()) {
        if (++silentRounds_ >= configuration_.silentRoundsBeforeReconnect) {
            reconnect("Leader stopped answering");
        }
    } else if (!roundProbes_.empty()) {
        silentRounds_ = 0;
    }

    ClockSyncRound result;
    result.index = round;
    result.localTime = hostClockNanoseconds();
    result.verdict = verdict;
    result.probesSent = roundProbesSent_;
    result.probesReceived = static_cast<int>(roundProbes_.size());
    if (!roundProbes_.empty()) {
        result.best = *std::min_element(roundProbes_.begin(), roundProbes_.end(), [](const ClockProbe& a, const ClockProbe& b) {
            return a.roundTrip() < b.roundTrip();
        });
    }
    if (estimator_.hasMap()) {
        result.map = estimator_.map();
        if (result.best) {
            result.residual = result.best->offset() - result.map->offsetAtLocal(result.best->localMidpoint());
        }
    }
    result.minimumRoundTrip = estimator_.minimumRoundTrip();
    result.residualRMS = estimator_.residualRMS();
    result.acceptedCount = estimator_.acceptedCount();
    result.rejectedCount = estimator_.rejectedCount();
    result.windowCount = estimator_.windowCount();
    if (onRound) onRound(result);

    sendHello();

    const int64_t interval = round < configuration_.warmupRounds ? configuration_.warmupInterval : configuration_.steadyInterval;
    queue_.asyncAfter(interval, [this] { startRound(); });
}

/// Sent outside the probe bursts so it never delays a ping.
void ClockSyncFollower::sendHello() {
    if (!connection_) return;
    const std::string name = utf8Prefix(identity_.name, kClockSyncMaximumNameLength);
    const Bytes hello = ClockSyncMessage::makeHello(identity_.id, Bytes(name.begin(), name.end())).encode();
    connection_->socket.send(hello.data(), hello.size());
}

void ClockSyncFollower::handle(const ClockSyncMessage& message, int64_t receivedAt, uint64_t connection) {
    if (!running_ || !connection_ || connection_->id != connection) return;
    // Ignores repeats, and older states that UDP delivered out of order.
    const auto isNew = [&message](std::optional<std::pair<uint64_t, uint32_t>>& current) {
        if (current && current->first == message.session && static_cast<int32_t>(message.sequence - current->second) <= 0) {
            return false;
        }
        current = std::make_pair(message.session, message.sequence);
        return true;
    };
    switch (message.type) {
    case ClockSyncMessage::Type::pong: {
        if (leaderSession_ != message.session) {
            leaderSession_ = message.session;
            estimator_.reset();
        }
        // Late replies from an earlier round would skew this round's minimum; drop them.
        if (message.sequence >> 8 != (static_cast<uint32_t>(roundIndex_) & 0xFF'FFFF)) return;
        roundProbes_.push_back(ClockProbe{message.t0, message.t1, message.t2, receivedAt});
        break;
    }
    case ClockSyncMessage::Type::playback:
        if (isNew(playbackRevision_) && onPlayback) onPlayback(message.playback);
        break;
    case ClockSyncMessage::Type::audioPort: {
        const net::Address endpoint = connection_->address.withPort(message.port);
        if (message.port == 0 || endpoint == audioEndpoint_) return;
        audioEndpoint_ = endpoint;
        if (onAudioEndpoint) onAudioEndpoint(endpoint);
        break;
    }
    case ClockSyncMessage::Type::speakers:
        if (isNew(speakersRevision_) && onSpeakers) onSpeakers(message.layout, message.channel, message.switchLeaderTime);
        break;
    case ClockSyncMessage::Type::ping:
    case ClockSyncMessage::Type::hello:
        break;
    }
}

}  // namespace asyncmusic
