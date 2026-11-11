//
//  Created by ktiays on 2026/11/11.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/share_center.hpp"

#include <algorithm>
#include <chrono>

namespace asyncmusic {

namespace {

constexpr int64_t kPollInterval = 200'000'000;

int64_t steadyNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

std::string ShareCenter::Share::summary() const {
    std::string source = info.source;
    if (source.empty()) {
        switch (info.sourceKind) {
        case ShareSourceKind::application: source = "App audio"; break;
        case ShareSourceKind::file: source = "Audio file"; break;
        case ShareSourceKind::system: source = "Computer audio"; break;
        default: source = "Audio"; break;
        }
    }
    switch (info.listenerCount) {
    case 0: return source;
    case 1: return source + " \xC2\xB7 1 listening";
    default: return source + " \xC2\xB7 " + std::to_string(info.listenerCount) + " listening";
    }
}

std::vector<ShareCenter::Share> ShareCenter::State::nearbyOthers() const {
    std::vector<Share> others;
    for (const Share& share : nearby) {
        if (!joined || share.id != joined->id) others.push_back(share);
    }
    return others;
}

ShareCenter::ShareCenter(GroupSession& session, Device device, Hooks hooks, ShareDiscoveryConfiguration configuration)
    : session_(session), device_(std::move(device)), hooks_(std::move(hooks)) {
    discovery_ = Ref<ShareDiscovery>::adopt(new ShareDiscovery(session.snapshot().identity.id, std::move(configuration)));
}

void ShareCenter::start() {
    // Nothing runs on the center's thread before this, so the callbacks set until now are
    // never read while being assigned.
    std::call_once(started_, [this] {
        queue_.async([this] {
            discovery_->listen();
            publish();
        });
        queue_.asyncAfter(kPollInterval, [this] { poll(); });
    });
}

void ShareCenter::enqueue(SerialQueue::Task task) {
    start();
    queue_.async(std::move(task));
}

ShareCenter::~ShareCenter() {
    queue_.shutdown();
    discovery_->stop();
}

// MARK: - Commands

void ShareCenter::setMode(Mode mode) {
    enqueue([this, mode] {
        apply(mode);
        publish();
    });
}

void ShareCenter::apply(Mode mode) {
    if (mode == mode_) return;
    if (joined_) leaveOnQueue();
    if (isSharing_) stopSharingOnQueue();
    mode_ = mode;
    alerts_.clear();
    if (mode == Mode::listen) {
        discovery_->listen();
    } else {
        // A device sharing its own audio does not take part in others' shares.
        discovery_->stop();
        nearby_.clear();
        seenShares_.clear();
        generation_ = 0;
    }
}

void ShareCenter::setAlertsEnabled(bool enabled) {
    enqueue([this, enabled] {
        if (alertsEnabled_ == enabled) return;
        alertsEnabled_ = enabled;
        if (!enabled && !alerts_.empty()) {
            alerts_.clear();
            hasUnseenShares_ = true;
        }
        publish();
    });
}

void ShareCenter::join(uint64_t id) {
    enqueue([this, id] {
        auto matches = [id](const Share& share) { return share.id == id; };
        auto found = std::find_if(nearby_.begin(), nearby_.end(), matches);
        if (found == nearby_.end()) {
            found = std::find_if(alerts_.begin(), alerts_.end(), matches);
            if (found == alerts_.end()) return;
        }
        const Share share = *found;
        joinOnQueue(share);
        publish();
    });
}

void ShareCenter::joinAddress(const std::string& name, const net::Address& address) {
    enqueue([this, name, address] {
        Share share;
        // Not a discovered share: an ID no announcement uses.
        share.id = 0;
        share.info.name = name;
        share.info.clockPort = address.port();
        share.address = address;
        share.firstSeen = steadyNow();
        joinOnQueue(share);
        publish();
    });
}

void ShareCenter::joinOnQueue(const Share& share) {
    if (mode_ != Mode::listen) apply(Mode::listen);
    alerts_.erase(std::remove_if(alerts_.begin(), alerts_.end(), [&](const Share& alert) { return alert.id == share.id; }), alerts_.end());
    if (joined_ && joined_->id == share.id && share.id != 0) return;
    setRole(GroupSession::Role::follower);
    const net::Address address = share.address;
    session_.follow(share.info.name, [address]() -> std::optional<net::Address> { return address; });
    joined_ = share;
}

void ShareCenter::leave() {
    enqueue([this] {
        leaveOnQueue();
        publish();
    });
}

void ShareCenter::leaveOnQueue() {
    if (!joined_) return;
    setRole(GroupSession::Role::off);
    joined_.reset();
}

void ShareCenter::requestChannel(SpeakerChannel channel) {
    start();
    session_.requestChannel(channel);
}

void ShareCenter::dismissAlerts() {
    enqueue([this] {
        if (alerts_.empty()) return;
        hasUnseenShares_ = true;
        alerts_.clear();
        publish();
    });
}

void ShareCenter::refresh() {
    enqueue([this] {
        hasUnseenShares_ = false;
        alerts_.clear();
        if (mode_ == Mode::listen) discovery_->refresh();
        publish();
    });
}

void ShareCenter::selectSource(Source source) {
    enqueue([this, source = std::move(source)] {
        if (isSharing_) return;
        source_ = source;
        publish();
    });
}

void ShareCenter::startSharing() {
    enqueue([this] {
        if (!source_ || isSharing_) return;
        if (mode_ != Mode::share) apply(Mode::share);
        setRole(GroupSession::Role::leader);
        isSharing_ = true;
        playsWhenLoaded_ = true;
        announced_.reset();
        publish();
    });
}

void ShareCenter::stopSharing() {
    enqueue([this] {
        stopSharingOnQueue();
        publish();
    });
}

void ShareCenter::stopSharingOnQueue() {
    if (!isSharing_) return;
    isSharing_ = false;
    playsWhenLoaded_ = false;
    announced_.reset();
    discovery_->stop();
    setRole(GroupSession::Role::off);
}

void ShareCenter::setLayout(SpeakerLayout layout) {
    start();
    session_.setSpeakerLayout(layout);
}

void ShareCenter::setRole(GroupSession::Role role) {
    if (hooks_.setRole) {
        hooks_.setRole(role);
    } else {
        session_.setRole(role);
    }
}

// MARK: - Polling

void ShareCenter::poll() {
    bool changed = false;

    const ShareDiscovery::Status status = discovery_->status();
    if (status.error != discoveryError_) {
        discoveryError_ = status.error;
        changed = true;
    }

    const GroupSession::Snapshot snapshot = session_.snapshot();
    if (snapshot.revision != lastSessionRevision_) {
        lastSessionRevision_ = snapshot.revision;
        changed = true;
    }

    if (isSharing_) {
        const bool loading = hooks_.isLoadingSource && hooks_.isLoadingSource();
        if (playsWhenLoaded_ && !loading && snapshot.role == GroupSession::Role::leader && snapshot.selectedTrack) {
            playsWhenLoaded_ = false;
            session_.play(snapshot.selectedTrack->id);
            changed = true;
        }
        updateAnnouncement(snapshot);
    }

    if (mode_ == Mode::listen) {
        const uint64_t current = discovery_->generation();
        if (current != generation_) {
            generation_ = current;
            readShares();
            changed = true;
        }
        // The share joined went away and its leader no longer answers: done. (A share joined
        // by address was never announced, so only its leader's silence counts.)
        if (joined_) {
            const bool announced = std::any_of(nearby_.begin(), nearby_.end(), [&](const Share& share) { return share.id == joined_->id; });
            const bool syncing = snapshot.followerState == ClockSyncFollower::State::syncing;
            const bool failed = snapshot.followerState == ClockSyncFollower::State::failed;
            if ((joined_->id != 0 && !announced && !syncing) || (joined_->id == 0 && failed)) {
                leaveOnQueue();
                changed = true;
            }
        }
    }

    if (changed) publish();
    queue_.asyncAfter(kPollInterval, [this] { poll(); });
}

void ShareCenter::readShares() {
    std::vector<Share> shares;
    for (const DiscoveredShare& discovered : discovery_->shares()) {
        Share share;
        share.id = discovered.share;
        share.device = discovered.device;
        share.info = discovered.info;
        share.address = discovered.address;
        share.firstSeen = discovered.firstSeen;
        shares.push_back(std::move(share));
    }
    // Oldest first, so the list doesn't reorder as shares are heard again.
    std::sort(shares.begin(), shares.end(), [](const Share& a, const Share& b) { return a.firstSeen != b.firstSeen ? a.firstSeen < b.firstSeen : a.id < b.id; });
    nearby_ = shares;
    if (joined_) {
        for (const Share& share : shares) {
            if (share.id == joined_->id) joined_ = share;
        }
    }

    std::vector<Share> fresh;
    for (const Share& share : shares) {
        if (seenShares_.count(share.id) == 0 && !(joined_ && joined_->id == share.id)) fresh.push_back(share);
        seenShares_.insert(share.id);
    }
    // Keep only alerts for shares still around, with their current details.
    std::vector<Share> alerts;
    for (const Share& alert : alerts_) {
        for (const Share& share : shares) {
            if (share.id == alert.id) alerts.push_back(share);
        }
    }
    alerts_ = alerts;
    if (fresh.empty()) return;
    if (!alertsEnabled_) {
        hasUnseenShares_ = true;
        return;
    }
    // Newest first.
    std::reverse(fresh.begin(), fresh.end());
    alerts_.insert(alerts_.begin(), fresh.begin(), fresh.end());
    if (onAlert) {
        for (const Share& share : fresh) onAlert(share);
    }
}

void ShareCenter::updateAnnouncement(const GroupSession::Snapshot& snapshot) {
    if (snapshot.role != GroupSession::Role::leader || snapshot.leaderPort == 0) return;
    ShareInfo info;
    info.name = snapshot.identity.name;
    info.source = source_ ? source_->name : std::string();
    info.sourceKind = source_ ? source_->kind : ShareSourceKind::unknown;
    info.deviceKind = device_.kind;
    info.model = device_.model;
    info.clockPort = snapshot.leaderPort;
    int listeners = 0;
    for (const ClockSyncLeader::Member& member : snapshot.members) {
        if (member.isActive) ++listeners;
    }
    info.listenerCount = static_cast<uint16_t>(std::min(listeners, 0xFFFF));
    if (announced_) {
        if (*announced_ != info) {
            discovery_->update(info);
            announced_ = info;
        }
    } else {
        discovery_->share(info);
        announced_ = info;
    }
}

// MARK: - State

void ShareCenter::publish() {
    const GroupSession::Snapshot snapshot = session_.snapshot();
    State state;
    state.mode = mode_;
    state.deviceName = snapshot.identity.name;
    state.nearby = nearby_;
    state.alerts = alerts_;
    state.hasUnseenShares = hasUnseenShares_;
    state.discoveryError = discoveryError_;

    state.joined = joined_;
    if (joined_) {
        state.isPlaying = snapshot.playback.isPlaying();
        state.channel = snapshot.channel;
        state.joinedLayout = snapshot.remoteLayout;
        switch (snapshot.followerState.value_or(ClockSyncFollower::State::connecting)) {
        case ClockSyncFollower::State::syncing: state.syncStatus = SyncStatus::syncing; break;
        case ClockSyncFollower::State::reconnecting: state.syncStatus = SyncStatus::reconnecting; break;
        case ClockSyncFollower::State::failed: state.syncStatus = SyncStatus::failed; break;
        default: state.syncStatus = SyncStatus::connecting; break;
        }
    }

    state.source = source_;
    state.isSharing = isSharing_;
    state.layout = snapshot.speakers.layout;
    if (isSharing_) {
        const std::string error = hooks_.sourceError ? hooks_.sourceError() : std::string();
        if (!error.empty()) {
            state.shareProblem = error;
        } else if ((hooks_.isLoadingSource && hooks_.isLoadingSource()) || (playsWhenLoaded_ && !snapshot.selectedTrack)) {
            state.shareProblem = "Loading\xE2\x80\xA6";
        } else if (snapshot.isPreparingPlayback) {
            state.shareProblem = "Starting\xE2\x80\xA6";
        } else if (!snapshot.playback.isPlaying()) {
            state.shareProblem = "Finished playing";
        }
        for (const ClockSyncLeader::Member& member : snapshot.members) {
            if (!member.isActive) continue;
            Listener listener;
            listener.id = member.id;
            listener.name = member.name;
            auto channel = snapshot.activeChannels.find(member.id);
            if (channel != snapshot.activeChannels.end() && channel->second != SpeakerChannel::full) listener.channel = channel->second;
            state.listeners.push_back(std::move(listener));
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        state.revision = state_.revision + 1;
        state_ = std::move(state);
    }
    if (onChange) onChange();
}

ShareCenter::State ShareCenter::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

}  // namespace asyncmusic
