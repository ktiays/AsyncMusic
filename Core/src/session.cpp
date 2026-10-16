//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/session.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "asyncmusic/host_clock.hpp"
#include "text.hpp"

namespace asyncmusic {

namespace {

constexpr size_t kHistoryLimit = 300;
constexpr size_t kLogLimit = 200;

const char* channelTitle(SpeakerChannel channel) {
    switch (channel) {
    case SpeakerChannel::full: return "the full mix";
    case SpeakerChannel::left: return "the left channel";
    case SpeakerChannel::right: return "the right channel";
    }
    return "?";
}

const char* channelName(SpeakerChannel channel) {
    switch (channel) {
    case SpeakerChannel::full: return "Full Mix";
    case SpeakerChannel::left: return "Left";
    case SpeakerChannel::right: return "Right";
    }
    return "?";
}

const char* layoutName(SpeakerLayout layout) {
    switch (layout) {
    case SpeakerLayout::everywhere: return "Everywhere";
    case SpeakerLayout::stereoPair: return "Stereo Pair";
    }
    return "?";
}

std::vector<SpeakerChannel> layoutChannels(SpeakerLayout layout) {
    std::vector<SpeakerChannel> channels;
    for (int i = 0; i < speakerLayoutChannelCount(layout); ++i) channels.push_back(speakerLayoutChannel(layout, i));
    return channels;
}

const char* verdictName(ClockVerdict verdict) {
    switch (verdict) {
    case ClockVerdict::accepted: return "ok";
    case ClockVerdict::noReplies: return "no replies";
    case ClockVerdict::slowRoundTrip: return "slow";
    case ClockVerdict::outlier: return "outlier";
    case ClockVerdict::reset: return "reset";
    }
    return "?";
}

}  // namespace

GroupSession::GroupSession(DeviceIdentity identity) : GroupSession(std::move(identity), SpeakerSettings{}) {}

GroupSession::GroupSession(DeviceIdentity identity, SpeakerSettings speakers)
    : queue_("asyncmusic.session"), player_([this](std::function<void()> work) { queue_.async(std::move(work)); }) {
    state_.identity = std::move(identity);
    state_.speakers = std::move(speakers);
    queue_.async([this] { tick(); });
}

GroupSession::~GroupSession() {
    queue_.sync([this] {
        running_ = false;
        tearDown();
    });
    queue_.shutdown();
}

GroupSession::Snapshot GroupSession::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string GroupSession::describe(uint64_t source) const {
    switch (source) {
    case kPlaybackSourceNone: return "nothing";
    case kPlaybackSourceMetronome: return "metronome";
    case kPlaybackSourceChannelTest: return "channel test";
    default: break;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto name = trackNames_.find(source);
    if (name != trackNames_.end()) return "“" + name->second + "”";
    return format("streamed file %016llx", static_cast<unsigned long long>(source));
}

bool GroupSession::Snapshot::isDevicePresent(uint64_t id) const {
    return id == identity.id || std::any_of(members.begin(), members.end(), [id](const auto& member) { return member.id == id && member.isActive; });
}

std::optional<std::string> GroupSession::Snapshot::nameOfDevice(uint64_t id) const {
    if (id == identity.id) return identity.name;
    for (const auto& member : members) {
        if (member.id == id) return member.name;
    }
    return std::nullopt;
}

void GroupSession::publish() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.playback = player_.playback();
        state_.channel = player_.channel();
        state_.clockMap = player_.clockMap();
        state_.isWaitingForAudio = player_.isWaitingForAudio();
        state_.isPreparingPlayback = pendingStart_ != 0;
        ++state_.revision;
    }
    if (onChange) onChange();
}

void GroupSession::append(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.log.push_back(LogLine{std::chrono::system_clock::now(), text});
    if (state_.log.size() > kLogLimit) {
        state_.log.erase(state_.log.begin(), state_.log.begin() + static_cast<std::ptrdiff_t>(state_.log.size() - kLogLimit));
    }
}

void GroupSession::clearLog() {
    queue_.async([this] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.log.clear();
        }
        publish();
    });
}

void GroupSession::tick() {
    if (!running_) return;
    player_.releaseRetiredBuffers();
    queue_.asyncAfter(1'000'000'000, [this] { tick(); });
}

// MARK: - Roles

void GroupSession::setRole(Role role) {
    queue_.async([this, role] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.role == role) return;
        }
        tearDown();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.role = role;
            state_.status = role == Role::follower ? "Looking for leaders…" : "Idle";
        }
        if (role == Role::leader) {
            // The leader's own clock is the group's timeline.
            player_.setClockMap(ClockMap::identity());
            startLeading();
        }
        publish();
    });
}

std::function<void(std::function<void()>)> GroupSession::onSession() {
    const uint64_t generation = generation_;
    return [this, generation](std::function<void()> work) {
        queue_.async([this, generation, work = std::move(work)] {
            if (running_ && generation == generation_) work();
        });
    };
}

void GroupSession::setIdentity(DeviceIdentity identity) {
    queue_.async([this, identity = std::move(identity)] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.identity = identity;
        }
        publish();
    });
}

void GroupSession::follow(const std::string& name, ClockSyncFollower::Resolver resolver) {
    queue_.async([this, name, resolver = std::move(resolver)]() mutable {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.role != Role::follower) return;
        }
        follower_.reset();
        audioClient_.reset();
        const uint64_t generation = ++generation_;
        player_.setClockMap(std::nullopt);
        player_.setPlayback(PlaybackState::stopped());
        player_.clearRemoteStream();
        resetRemoteSpeakers();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_.rounds.clear();
            state_.latestRound.reset();
            state_.connectedLeader = name;
            state_.followerState.reset();
            state_.isAudioConnected = false;
        }
        append("Following " + name);

        DeviceIdentity identity;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            identity = state_.identity;
        }
        auto follower = std::make_unique<ClockSyncFollower>(std::move(resolver), identity);
        // Every callback hops to the session's thread and is dropped if the follower was
        // replaced meanwhile.
        const auto onSession = [this, generation](std::function<void()> work) {
            queue_.async([this, generation, work = std::move(work)] {
                if (running_ && generation == generation_) work();
            });
        };
        follower->onStateChange = [this, onSession](ClockSyncFollower::State state, const std::string& reason) {
            onSession([this, state, reason] { followerStateChanged(state, reason); });
        };
        follower->onRound = [this, onSession](const ClockSyncRound& round) {
            onSession([this, round] { record(round); });
        };
        follower->onPlayback = [this, onSession](const PlaybackState& state) {
            onSession([this, state] { applyRemotePlayback(state); });
        };
        follower->onAudioEndpoint = [this, onSession](const net::Address& endpoint) {
            onSession([this, endpoint] { connectAudioStream(endpoint); });
        };
        follower->onSpeakers = [this, onSession](SpeakerLayout layout, SpeakerChannel channel, int64_t switchTime) {
            onSession([this, layout, channel, switchTime] { applyRemoteSpeakers(layout, channel, switchTime); });
        };
        follower_ = std::move(follower);
        follower_->start();
        publish();
    });
}

void GroupSession::tearDown() {
    ++generation_;
    pendingStart_ = 0;
    ++endOfTrack_;
    // The stream goes before the server it feeds, and the server before the leader it reports to.
    player_.onOutgoingStream = nullptr;
    outgoingStream_.reset();
    audioServer_.reset();
    leader_.reset();
    announcedLayout_.reset();
    follower_.reset();
    audioClient_.reset();
    player_.setPlayback(PlaybackState::stopped());
    player_.setClockMap(std::nullopt);
    player_.clearRemoteStream();
    resetRemoteSpeakers();
    std::lock_guard<std::mutex> lock(mutex_);
    state_.connectedLeader.reset();
    state_.followerState.reset();
    state_.isAudioConnected = false;
    state_.latestRound.reset();
    state_.rounds.clear();
    state_.leaderPort = 0;
    state_.followerCount = 0;
    state_.members.clear();
    state_.activeChannels.clear();
    state_.isLayoutComplete = false;
}

// MARK: - Leader

void GroupSession::startLeading() {
    const auto post = onSession();
    auto leader = std::make_unique<ClockSyncLeader>();
    leader->onStateChange = [this, post](ClockSyncLeader::State state, const std::string& reason) {
        post([this, state, reason] { leaderStateChanged(state, reason); });
    };
    leader->onPlaybackReset = [this, post] {
        post([this] {
            player_.setPlayback(PlaybackState::stopped());
            append("This device slept; playback stopped and followers will resync");
            publish();
        });
    };
    leader->onFollowersChange = [this, post](int count) {
        post([this, count] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (count == state_.followerCount) return;
                state_.followerCount = count;
            }
            append(format("Followers: %d", count));
            publish();
        });
    };
    leader->onMembersChange = [this, post](const std::vector<ClockSyncLeader::Member>& members) {
        post([this, members] {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.members = members;
            }
            updateSpeakers();
            publish();
        });
    };
    leader_ = std::move(leader);
    leader_->start();
    updateSpeakers();

    auto server = std::make_unique<AudioStreamServer>();
    ClockSyncLeader* clock = leader_.get();
    // The server is always stopped before the leader goes away.
    server->onPortChange = [clock](uint16_t port) { clock->setAudioPort(port); };
    server->onReceiversChange = [this, post](int count) {
        post([this, count] {
            append(format("Audio stream receivers: %d", count));
            publish();
        });
    };
    server->onError = [this, post](const std::string& message) {
        post([this, message] {
            append("Audio stream error: " + message);
            publish();
        });
    };
    player_.onOutgoingStream = [this](const std::optional<OutgoingStream>& stream) {
        outgoingStream_ = stream;
        if (audioServer_) audioServer_->setStream(stream);
    };
    audioServer_ = std::move(server);
    audioServer_->start();
}

void GroupSession::leaderStateChanged(ClockSyncLeader::State state, const std::string& reason) {
    std::string name;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        name = state_.identity.name;
        switch (state) {
        case ClockSyncLeader::State::starting: state_.status = "Starting listener…"; break;
        case ClockSyncLeader::State::advertising:
            state_.status = "Advertising as “" + name + "”";
            state_.leaderPort = leader_ ? leader_->port() : 0;
            break;
        case ClockSyncLeader::State::failed:
            state_.status = "Failed: " + reason;
            state_.leaderPort = 0;
            break;
        case ClockSyncLeader::State::stopped: state_.leaderPort = 0; break;
        }
    }
    if (state == ClockSyncLeader::State::advertising && leader_) append(format("Advertising on UDP port %u", leader_->port()));
    if (state == ClockSyncLeader::State::failed) append("Leader error: " + reason);
    publish();
}

void GroupSession::addTrack(std::shared_ptr<const MediaFile> file) {
    queue_.async([this, file = std::move(file)] {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            trackNames_[file->id] = file->name;
            state_.selectedTrack = Track{file->id, file->name, file->duration()};
        }
        append(format("Loaded “%s” (%.1fs, id %016llx)", file->name.c_str(), file->duration(), static_cast<unsigned long long>(file->id)));
        player_.add(file);
        publish();
    });
}

void GroupSession::play(uint64_t source) {
    queue_.async([this, source] {
        if (!leader_) return;
        pendingStart_ = 0;
        if (isGeneratedSource(source)) {
            const PlaybackState state{source, hostClockNanoseconds() + kStartLeadTime, 0};
            player_.setPlayback(state);
            leader_->setPlayback(state);
            append(format("Play %s in %.1fs", describe(source).c_str(), static_cast<double>(kStartLeadTime) / 1e9));
            publish();
            return;
        }

        // Start decoding (and with it streaming) right away, provisionally scheduled after the
        // longest wait. Followers buffer the stream without being told to play yet.
        const int64_t requested = hostClockNanoseconds();
        const PlaybackState provisional{source, requested + static_cast<int64_t>(kMaximumStartWait * 1e9) + kStartMargin, 0};
        player_.setPlayback(provisional);
        if (!outgoingStream_ || outgoingStream_->source != source || !audioServer_) {
            leader_->setPlayback(provisional);
            scheduleEndOfTrack(provisional);
            publish();
            return;
        }
        const uint64_t token = ++playRequests_;
        pendingStart_ = token;
        append("Buffering " + describe(source) + " on followers…");
        publish();

        const StreamBuffer& buffer = *outgoingStream_->buffer;
        const int64_t threshold = buffer.validRange().lowerBound + static_cast<int64_t>(kStartBufferDuration * buffer.sampleRate());
        const auto post = onSession();
        audioServer_->waitUntilBuffered(outgoingStream_->id, threshold, kMaximumStartWait, [this, post, token, source, requested](AudioStreamServer::Readiness readiness) {
            post([this, token, source, requested, readiness] {
                if (pendingStart_ != token || !leader_) return;
                pendingStart_ = 0;
                const PlaybackState state{source, hostClockNanoseconds() + kStartMargin, 0};
                player_.setPlayback(state);
                leader_->setPlayback(state);
                scheduleEndOfTrack(state);
                std::string line = format("Play %s: %d/%d followers ready, starts %.2fs after request", describe(source).c_str(), readiness.ready, readiness.total,
                    static_cast<double>(state.startLeaderTime - requested) / 1e9);
                if (readiness.timedOut) line += " (gave up waiting)";
                append(line);
                publish();
            });
        });
    });
}

void GroupSession::stopPlayback() {
    queue_.async([this] {
        if (!leader_) return;
        pendingStart_ = 0;
        ++endOfTrack_;
        leader_->setPlayback(PlaybackState::stopped());
        player_.setPlayback(PlaybackState::stopped());
        append("Stop");
        publish();
    });
}

/// Stops the group once the file in `state` has played to its end.
void GroupSession::scheduleEndOfTrack(const PlaybackState& state) {
    const uint64_t token = ++endOfTrack_;
    const auto file = player_.file(state.source);
    if (!file) return;
    // The leader's own clock is the group's timeline.
    const int64_t end = state.startLeaderTime - state.startMediaTime + static_cast<int64_t>(file->duration() * 1e9);
    queue_.asyncAfter(std::max<int64_t>(0, end - hostClockNanoseconds()) + 300'000'000, [this, token, state] {
        if (!running_ || token != endOfTrack_ || !leader_ || player_.playback() != state) return;
        append("Finished " + describe(state.source));
        pendingStart_ = 0;
        ++endOfTrack_;
        leader_->setPlayback(PlaybackState::stopped());
        player_.setPlayback(PlaybackState::stopped());
        publish();
    });
}

// MARK: - Speakers

void GroupSession::setSpeakerLayout(SpeakerLayout layout) {
    queue_.async([this, layout] {
        SpeakerSettings settings;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_.speakers.layout == layout) return;
            settings = state_.speakers;
        }
        settings.layout = layout;
        storeSpeakers(settings);
        append(std::string("Speaker layout: ") + layoutName(layout));
        updateSpeakers();
        publish();
    });
}

void GroupSession::assign(std::optional<uint64_t> device, SpeakerChannel channel) {
    queue_.async([this, device, channel] {
        std::map<SpeakerChannel, uint64_t> assignments;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            assignments = state_.speakers.assignments;
        }
        const auto previous = assignments.find(channel);
        const std::optional<uint64_t> replaced = previous == assignments.end() ? std::nullopt : std::optional<uint64_t>(previous->second);
        if (device) {
            for (auto& [other, assigned] : std::map<SpeakerChannel, uint64_t>(assignments)) {
                if (assigned == *device && other != channel) {
                    if (replaced) assignments[other] = *replaced;
                    else assignments.erase(other);
                }
            }
            assignments[channel] = *device;
        } else {
            assignments.erase(channel);
        }
        setAssignments(assignments);
    });
}

void GroupSession::swapChannels() {
    queue_.async([this] {
        std::map<SpeakerChannel, uint64_t> assignments;
        SpeakerLayout layout;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            assignments = state_.speakers.assignments;
            layout = state_.speakers.layout;
        }
        const auto channels = layoutChannels(layout);
        if (channels.size() != 2) return;
        const auto first = assignments.find(channels[0]);
        const auto second = assignments.find(channels[1]);
        const std::optional<uint64_t> a = first == assignments.end() ? std::nullopt : std::optional<uint64_t>(first->second);
        const std::optional<uint64_t> b = second == assignments.end() ? std::nullopt : std::optional<uint64_t>(second->second);
        assignments.erase(channels[0]);
        assignments.erase(channels[1]);
        if (b) assignments[channels[0]] = *b;
        if (a) assignments[channels[1]] = *a;
        setAssignments(assignments);
    });
}

void GroupSession::setAssignments(const std::map<SpeakerChannel, uint64_t>& assignments) {
    SpeakerSettings settings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_.speakers.assignments == assignments) return;
        settings = state_.speakers;
    }
    settings.assignments = assignments;
    storeSpeakers(settings);
    updateSpeakers();
    publish();
}

void GroupSession::storeSpeakers(const SpeakerSettings& settings) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.speakers = settings;
    }
    if (onSpeakerSettingsChange) onSpeakerSettingsChange(settings);
}

/// Fills channels that have no device in the group with the obvious choice: this device first,
/// then a follower if exactly one is left over. That pairs the common two-device setup without
/// asking, and keeps a pairing once made.
void GroupSession::fillOpenChannels() {
    Snapshot state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    auto assignments = state.speakers.assignments;
    std::set<uint64_t> known{state.identity.id};
    for (const auto& member : state.members) known.insert(member.id);
    const auto channels = layoutChannels(state.speakers.layout);
    std::vector<SpeakerChannel> open;
    std::set<uint64_t> assigned;
    for (const auto channel : channels) {
        const auto device = assignments.find(channel);
        if (device == assignments.end() || known.count(device->second) == 0) open.push_back(channel);
        if (device != assignments.end()) assigned.insert(device->second);
    }
    if (open.empty()) return;
    if (assigned.count(state.identity.id) == 0) {
        assignments[open.front()] = state.identity.id;
        open.erase(open.begin());
    }
    std::vector<ClockSyncLeader::Member> spare;
    for (const auto& member : state.members) {
        if (member.isActive && assigned.count(member.id) == 0) spare.push_back(member);
    }
    if (open.size() == 1 && spare.size() == 1) {
        assignments[open.front()] = spare.front().id;
        std::string side = channelName(open.front());
        std::transform(side.begin(), side.end(), side.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        append("Paired " + spare.front().name + " as the " + side + " speaker");
    }
    if (assignments != state.speakers.assignments) {
        SpeakerSettings settings = state.speakers;
        settings.assignments = assignments;
        storeSpeakers(settings);
    }
}

/// Works out every device's channel and, if anything changed, tells the group. The switch is
/// scheduled slightly ahead so all devices make it at the same instant.
void GroupSession::updateSpeakers() {
    if (!leader_) return;
    const auto isComplete = [](const Snapshot& state) {
        for (const auto channel : layoutChannels(state.speakers.layout)) {
            const auto device = state.speakers.assignments.find(channel);
            if (device == state.speakers.assignments.end() || !state.isDevicePresent(device->second)) return false;
        }
        return true;
    };
    Snapshot state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    const auto channels = layoutChannels(state.speakers.layout);
    if (!channels.empty() && !isComplete(state)) {
        fillOpenChannels();
        std::lock_guard<std::mutex> lock(mutex_);
        state = state_;
    }
    const bool complete = isComplete(state);
    std::map<uint64_t, SpeakerChannel> active;
    if (complete) {
        for (const auto channel : channels) active[state.speakers.assignments.at(channel)] = channel;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.isLayoutComplete = complete;
        if (active == state_.activeChannels && announcedLayout_ == state.speakers.layout) return;
        state_.activeChannels = active;
    }
    announcedLayout_ = state.speakers.layout;
    const int64_t switchTime = hostClockNanoseconds() + kChannelSwitchLead;
    leader_->setSpeakers(state.speakers.layout, active, switchTime);
    const auto own = active.find(state.identity.id);
    player_.setChannel(own == active.end() ? SpeakerChannel::full : own->second, switchTime);
    if (!channels.empty()) {
        if (active.empty()) {
            append("Not all speakers present; everyone plays the full mix");
        } else {
            std::string summary = "Speakers: ";
            for (size_t i = 0; i < channels.size(); ++i) {
                const auto device = state.speakers.assignments.find(channels[i]);
                const auto name = device == state.speakers.assignments.end() ? std::nullopt : state.nameOfDevice(device->second);
                summary += std::string(i > 0 ? ", " : "") + channelName(channels[i]) + ": " + name.value_or("–");
            }
            append(summary);
        }
    }
}

// MARK: - Follower

/// (Re)connects the audio stream once the clock connection learned where it is.
void GroupSession::connectAudioStream(const net::Address& endpoint) {
    audioClient_.reset();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.isAudioConnected = false;
    }
    append("Audio stream at " + endpoint.toString());
    auto client = std::make_unique<AudioStreamClient>(endpoint);
    // Late callbacks from a replaced client (or follower) are dropped.
    const uint64_t generation = generation_;
    const uint64_t audioGeneration = ++audioGeneration_;
    const auto onSession = [this, generation, audioGeneration](std::function<void()> work) {
        queue_.async([this, generation, audioGeneration, work = std::move(work)] {
            if (running_ && generation == generation_ && audioGeneration == audioGeneration_) work();
        });
    };
    client->onStateChange = [this, onSession](AudioStreamClient::State state, const std::string& reason) {
        onSession([this, state, reason] {
            switch (state) {
            case AudioStreamClient::State::connected: append("Audio stream connected"); break;
            case AudioStreamClient::State::reconnecting: append("Audio stream reconnecting: " + reason); break;
            case AudioStreamClient::State::connecting:
            case AudioStreamClient::State::stopped: break;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                state_.isAudioConnected = state == AudioStreamClient::State::connected;
            }
            publish();
        });
    };
    client->onStream = [this, onSession](uint64_t source, const Ref<StreamBuffer>& buffer) {
        onSession([this, source, buffer] {
            player_.setRemoteStream(buffer, source);
            publish();
        });
    };
    client->onLog = [this, onSession](const std::string& text) {
        onSession([this, text] {
            append(text);
            publish();
        });
    };
    audioClient_ = std::move(client);
    audioClient_->start();
    publish();
}

void GroupSession::followerStateChanged(ClockSyncFollower::State state, const std::string& reason) {
    std::string leader;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.followerState = state;
        leader = state_.connectedLeader.value_or("leader");
        switch (state) {
        case ClockSyncFollower::State::connecting: state_.status = "Connecting to " + leader + "…"; break;
        case ClockSyncFollower::State::syncing: state_.status = "Syncing with " + leader; break;
        case ClockSyncFollower::State::reconnecting: state_.status = "Reconnecting to " + leader + "…"; break;
        case ClockSyncFollower::State::failed: state_.status = "Failed: " + reason; break;
        case ClockSyncFollower::State::stopped: break;
        }
    }
    switch (state) {
    case ClockSyncFollower::State::syncing: append("Connected"); break;
    case ClockSyncFollower::State::reconnecting: append("Reconnecting: " + reason); break;
    case ClockSyncFollower::State::failed: append("Follower error: " + reason); break;
    case ClockSyncFollower::State::connecting:
    case ClockSyncFollower::State::stopped: break;
    }
    publish();
}

void GroupSession::record(const ClockSyncRound& round) {
    player_.setClockMap(round.map);
    std::string line = format("#%d %d/%d ", round.index, round.probesReceived, round.probesSent);
    line += round.best ? format("rtt %.2fms", static_cast<double>(round.best->roundTrip()) / 1e6) : std::string("no replies");
    line += std::string(" ") + verdictName(round.verdict);
    if (round.map) {
        line += format(" offset %.3fms skew %+.2fppm", static_cast<double>(round.map->offsetAtLocal(round.localTime)) / 1e6, round.map->skewPPM());
    }
    if (round.residual) {
        line += format(" res %+.0fµs", static_cast<double>(*round.residual) / 1e3);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.latestRound = round;
        state_.rounds.push_back(round);
        if (state_.rounds.size() > kHistoryLimit) {
            state_.rounds.erase(state_.rounds.begin(), state_.rounds.begin() + static_cast<std::ptrdiff_t>(state_.rounds.size() - kHistoryLimit));
        }
    }
    append(line);
    publish();
}

void GroupSession::applyRemotePlayback(const PlaybackState& state) {
    player_.setPlayback(state);
    if (state.isPlaying()) {
        const auto& map = player_.clockMap();
        const int64_t now = map ? map->leaderTimeFromLocal(hostClockNanoseconds()) : state.startLeaderTime;
        append(format("Leader plays %s (start in %.3fs)", describe(state.source).c_str(), static_cast<double>(state.startLeaderTime - now) / 1e9));
    } else {
        append("Leader stopped");
    }
    publish();
}

void GroupSession::applyRemoteSpeakers(SpeakerLayout layout, SpeakerChannel channel, int64_t switchTime) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.remoteLayout = layout;
    }
    if (channel != player_.channel()) {
        append(std::string("This device now plays ") + channelTitle(channel));
    }
    player_.setChannel(channel, switchTime);
    publish();
}

void GroupSession::resetRemoteSpeakers() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.remoteLayout = SpeakerLayout::everywhere;
    }
    player_.setChannel(SpeakerChannel::full);
}

}  // namespace asyncmusic
