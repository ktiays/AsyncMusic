//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/session.hpp"

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

GroupSession::GroupSession(DeviceIdentity identity) : queue_("asyncmusic.session") {
    state_.identity = std::move(identity);
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

std::string GroupSession::describe(uint64_t source) {
    switch (source) {
    case kPlaybackSourceNone: return "nothing";
    case kPlaybackSourceMetronome: return "metronome";
    case kPlaybackSourceChannelTest: return "channel test";
    default: return format("streamed file %016llx", static_cast<unsigned long long>(source));
    }
}

void GroupSession::publish() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_.playback = player_.playback();
        state_.channel = player_.channel();
        state_.clockMap = player_.clockMap();
        state_.isWaitingForAudio = player_.isWaitingForAudio();
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
        if (role == Role::leader) {
            append("Hosting is not available on this device yet");
            publish();
            return;
        }
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
        publish();
    });
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
