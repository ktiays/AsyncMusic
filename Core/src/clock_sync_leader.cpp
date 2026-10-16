//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include <algorithm>
#include <random>

#include "asyncmusic/clock_sync.hpp"
#include "asyncmusic/host_clock.hpp"
#include "asyncmusic/protocol.hpp"

namespace asyncmusic {

namespace {

/// A follower silent for longer than this counts as gone for its speaker channel; followers
/// ping at least once a second.
constexpr int64_t kActiveWindow = 3'000'000'000;
/// Followers silent for this long are dropped altogether.
constexpr int64_t kDropAfter = 10'000'000'000;

uint64_t randomSession() {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32) ^ device();
}

}  // namespace

ClockSyncLeader::ClockSyncLeader() : queue_("asyncmusic.clock-sync.leader"), session_(randomSession()) {}

ClockSyncLeader::~ClockSyncLeader() {
    stop();
    queue_.shutdown();
}

void ClockSyncLeader::start() {
    queue_.async([this] {
        if (running_) return;
        running_ = true;
        lastSleepDuration_ = sleptNanoseconds();
        if (onStateChange) onStateChange(State::starting, "");
        listen();
        queue_.asyncAfter(1'000'000'000, [this] { tick(); });
    });
}

void ClockSyncLeader::stop() {
    queue_.sync([this] {
        if (!running_) return;
        running_ = false;
        receiving_ = false;
        if (receiver_.joinable()) receiver_.join();
        socket_.close();
        port_ = 0;
        peers_.clear();
        reportMembers();
        if (onFollowersChange) onFollowersChange(0);
        if (onStateChange) onStateChange(State::stopped, "");
    });
}

void ClockSyncLeader::listen() {
    if (!running_ || socket_.isOpen()) return;
    net::startup();
    // Dual-stack, so followers reach it whichever address mDNS gave them; IPv4 alone otherwise.
    const bool open = (socket_.open(net::Address::fromString("::", 0)->family()) && socket_.bind(0))
        || (socket_.open(net::Address::fromString("0.0.0.0", 0)->family()) && socket_.bind(0));
    if (!open) {
        const std::string error = net::lastErrorDescription();
        socket_.close();
        if (onStateChange) onStateChange(State::failed, error);
        queue_.asyncAfter(1'000'000'000, [this] { listen(); });
        return;
    }
    socket_.setVoiceTrafficClass();
    port_ = socket_.localAddress().port();
    receiving_ = true;
    receiver_ = std::thread([this] { receive(); });
    if (onStateChange) onStateChange(State::advertising, "");
}

void ClockSyncLeader::receive() {
    uint8_t buffer[512];
    while (receiving_.load(std::memory_order_relaxed)) {
        net::Address from;
        const int size = socket_.receive(buffer, sizeof(buffer), 100, &from);
        // Stamp before doing anything else: any delay between the packet arriving and this line
        // shows up as path asymmetry.
        const int64_t t1 = hostClockNanoseconds();
        ClockSyncMessage message;
        if (size <= 0 || !ClockSyncMessage::decode(buffer, static_cast<size_t>(size), message)) continue;
        if (message.type == ClockSyncMessage::Type::ping) {
            // Answered right here, stamped as late as possible; the turnaround is subtracted out.
            const Bytes pong = ClockSyncMessage::makePong(message.sequence, session_.load(), message.t0, t1, hostClockNanoseconds()).encode();
            socket_.sendTo(pong.data(), pong.size(), from);
        }
        queue_.async([this, from, t1, message = std::move(message)] { heard(from, t1, message); });
    }
}

void ClockSyncLeader::heard(const net::Address& from, int64_t time, const ClockSyncMessage& message) {
    if (!running_) return;
    const std::string key = from.toString();
    auto peer = peers_.find(key);
    if (peer == peers_.end()) {
        Peer added;
        added.address = from;
        peer = peers_.emplace(key, std::move(added)).first;
        if (onFollowersChange) onFollowersChange(static_cast<int>(peers_.size()));
        sendPlayback(from);
    }
    peer->second.lastHeard = time;
    if (message.type != ClockSyncMessage::Type::hello) return;

    // Every hello is answered with the full state: a follower then stays current even when
    // the periodic broadcast cannot reach it.
    const std::string name(message.name.begin(), message.name.end());
    if (!peer->second.hasDevice || peer->second.device != message.device || peer->second.name != name) {
        peer->second.hasDevice = true;
        peer->second.device = message.device;
        peer->second.name = name;
        reportMembers();
    }
    sendPlayback(from);
    sendSpeakers(peer->second);
}

void ClockSyncLeader::tick() {
    if (!running_) return;
    // A device that slept has a host clock that stood still meanwhile: start a new timeline.
    const int64_t slept = sleptNanoseconds();
    if (slept - lastSleepDuration_ > kSleepDetectionThreshold) {
        lastSleepDuration_ = slept;
        session_ = randomSession();
        // Silence over the new timeline would be ambiguous; announce stopped playback.
        playback_ = PlaybackState::stopped();
        ++playbackRevision_;
        if (onPlaybackReset) onPlaybackReset();
    }
    const int64_t cutoff = hostClockNanoseconds() - kDropAfter;
    bool dropped = false;
    for (auto peer = peers_.begin(); peer != peers_.end();) {
        if (peer->second.lastHeard < cutoff) {
            peer = peers_.erase(peer);
            dropped = true;
        } else {
            ++peer;
        }
    }
    if (dropped && onFollowersChange) onFollowersChange(static_cast<int>(peers_.size()));
    broadcast();
    reportMembers();
    queue_.asyncAfter(1'000'000'000, [this] { tick(); });
}

void ClockSyncLeader::setPlayback(const PlaybackState& state) {
    queue_.async([this, state] {
        playback_ = state;
        ++playbackRevision_;
        broadcast();
    });
}

void ClockSyncLeader::setAudioPort(uint16_t port) {
    queue_.async([this, port] {
        audioPort_ = port;
        broadcast();
    });
}

void ClockSyncLeader::setSpeakers(SpeakerLayout layout, const std::map<uint64_t, SpeakerChannel>& channels, int64_t switchTime) {
    queue_.async([this, layout, channels, switchTime] {
        layout_ = layout;
        channels_ = channels;
        channelSwitchTime_ = switchTime;
        ++speakersRevision_;
        for (const auto& [key, peer] : peers_) sendSpeakers(peer);
    });
}

void ClockSyncLeader::broadcast() {
    for (const auto& [key, peer] : peers_) {
        sendPlayback(peer.address);
        sendSpeakers(peer);
    }
}

/// The playback state, and the audio stream's port along with it.
void ClockSyncLeader::sendPlayback(const net::Address& to) {
    send(ClockSyncMessage::makePlayback(playbackRevision_, session_.load(), playback_), to);
    if (audioPort_ != 0) send(ClockSyncMessage::makeAudioPort(session_.load(), audioPort_), to);
}

/// The follower's channel, once it has said who it is.
void ClockSyncLeader::sendSpeakers(const Peer& peer) {
    if (!peer.hasDevice) return;
    const auto channel = channels_.find(peer.device);
    send(ClockSyncMessage::makeSpeakers(speakersRevision_, session_.load(), layout_, channel == channels_.end() ? SpeakerChannel::full : channel->second,
             channelSwitchTime_),
        peer.address);
}

void ClockSyncLeader::send(const ClockSyncMessage& message, const net::Address& to) {
    if (!socket_.isOpen()) return;
    const Bytes bytes = message.encode();
    socket_.sendTo(bytes.data(), bytes.size(), to);
}

void ClockSyncLeader::reportMembers() {
    const int64_t cutoff = hostClockNanoseconds() - kActiveWindow;
    std::map<uint64_t, Member> members;
    for (const auto& [key, peer] : peers_) {
        if (!peer.hasDevice) continue;
        // A follower that reconnected briefly has two addresses; either one counts.
        auto& member = members[peer.device];
        member.id = peer.device;
        member.name = peer.name;
        member.isActive = member.isActive || peer.lastHeard >= cutoff;
    }
    std::vector<Member> sorted;
    for (const auto& [id, member] : members) sorted.push_back(member);
    std::sort(sorted.begin(), sorted.end(), [](const Member& a, const Member& b) { return a.name != b.name ? a.name < b.name : a.id < b.id; });
    if (sorted == reportedMembers_) return;
    reportedMembers_ = sorted;
    if (onMembersChange) onMembersChange(sorted);
}

}  // namespace asyncmusic
