//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "Engine.h"

#include <asyncmusic/host_clock.hpp>

#include <chrono>

#include "Json.h"
#include "MediaLoader.h"

namespace asyncmusic::android {

namespace {

const char* roleName(GroupSession::Role role) {
    switch (role) {
    case GroupSession::Role::off: return "off";
    case GroupSession::Role::leader: return "leader";
    case GroupSession::Role::follower: return "follower";
    }
    return "off";
}

const char* followerStateName(ClockSyncFollower::State state) {
    switch (state) {
    case ClockSyncFollower::State::connecting: return "connecting";
    case ClockSyncFollower::State::syncing: return "syncing";
    case ClockSyncFollower::State::reconnecting: return "reconnecting";
    case ClockSyncFollower::State::failed: return "failed";
    case ClockSyncFollower::State::stopped: return "stopped";
    }
    return "stopped";
}

const char* verdictName(ClockVerdict verdict) {
    switch (verdict) {
    case ClockVerdict::accepted: return "accepted";
    case ClockVerdict::noReplies: return "no replies";
    case ClockVerdict::slowRoundTrip: return "slow round trip";
    case ClockVerdict::outlier: return "outlier";
    case ClockVerdict::reset: return "reset";
    }
    return "?";
}

void writeStats(JsonWriter& json, const RenderStats& stats) {
    json.beginObject();
    json.key("hostTimeValid").value(stats.hostTimeValid);
    json.key("isPlaying").value(stats.isPlaying);
    json.key("errorMicroseconds").value(stats.errorMicroseconds);
    json.key("correctionPPM").value(stats.correctionPPM);
    json.key("jumps").value(stats.jumps);
    json.key("mediaSeconds").value(stats.mediaSeconds);
    json.key("bufferedSeconds").value(stats.bufferedSeconds);
    json.key("gaps").value(stats.gaps);
    json.key("outputPeak").value(stats.outputPeak);
    json.endObject();
}

void writeOutput(JsonWriter& json, const AudioOutput::Status& status, double manualOffset) {
    json.beginObject();
    json.key("isRunning").value(status.isRunning);
    json.key("error").value(status.error);
    json.key("deviceID").value(status.deviceID);
    json.key("sampleRate").value(status.sampleRate);
    json.key("channels").value(status.channels);
    json.key("bufferDuration").value(status.bufferDuration);
    json.key("burstDuration").value(status.burstDuration);
    json.key("latency").value(status.latency);
    json.key("timestampJitter").value(status.timestampJitter);
    json.key("lowLatency").value(status.lowLatency);
    json.key("underruns").value(status.underruns);
    json.key("manualOffset").value(manualOffset);
    json.endObject();
}

void writeRound(JsonWriter& json, const ClockSyncRound& round) {
    json.beginObject();
    json.key("index").value(round.index);
    json.key("verdict").value(verdictName(round.verdict));
    json.key("accepted").value(round.verdict == ClockVerdict::accepted);
    json.key("probesSent").value(round.probesSent);
    json.key("probesReceived").value(round.probesReceived);
    json.key("bestRoundTrip");
    if (round.best) json.value(round.best->roundTrip()); else json.null();
    json.key("residual");
    if (round.residual) json.value(*round.residual); else json.null();
    json.key("offset");
    if (round.map) json.value(round.map->offsetAtLocal(round.localTime)); else json.null();
    json.key("skewPPM");
    if (round.map) json.value(round.map->skewPPM()); else json.null();
    json.key("minimumRoundTrip").value(round.minimumRoundTrip);
    json.key("residualRMS").value(round.residualRMS);
    json.key("acceptedCount").value(round.acceptedCount);
    json.key("rejectedCount").value(round.rejectedCount);
    json.key("windowCount").value(round.windowCount);
    json.endObject();
}

/// The leader's clock now, if known, in nanoseconds.
void writeLeaderClock(JsonWriter& json, const std::optional<ClockMap>& map) {
    json.key("leaderClock");
    if (map) json.value(map->leaderTimeFromLocal(hostClockNanoseconds())); else json.null();
}

const char* syncStatusName(ShareCenter::SyncStatus status) {
    switch (status) {
    case ShareCenter::SyncStatus::connecting: return "connecting";
    case ShareCenter::SyncStatus::syncing: return "syncing";
    case ShareCenter::SyncStatus::reconnecting: return "reconnecting";
    case ShareCenter::SyncStatus::failed: return "failed";
    }
    return "connecting";
}

void writeShare(JsonWriter& json, const ShareCenter::Share& share) {
    json.beginObject();
    json.key("id").id(share.id);
    json.key("device").id(share.device);
    json.key("name").value(share.info.name);
    json.key("source").value(share.info.source);
    json.key("sourceKind").value(static_cast<int>(share.info.sourceKind));
    json.key("deviceKind").value(static_cast<int>(share.info.deviceKind));
    json.key("model").value(share.info.model);
    json.key("listenerCount").value(static_cast<int>(share.info.listenerCount));
    json.key("address").value(share.address.toString());
    json.key("summary").value(share.summary());
    json.endObject();
}

}  // namespace

Engine::Engine(Options options, Callbacks callbacks) : callbacks_(std::move(callbacks)) {
    session_ = std::make_unique<GroupSession>(std::move(options.identity), std::move(options.speakers));
    output_ = std::make_unique<AudioOutput>(session_->renderer());
    output_->setManualOffset(options.manualOffset);
    output_->setVolume(options.volume);
    session_->onSpeakerSettingsChange = [this](const GroupSession::SpeakerSettings& settings) {
        if (callbacks_.onSpeakerSettingsChange) callbacks_.onSpeakerSettingsChange(settings);
    };
    session_->onChange = [this] {
        if (callbacks_.onChange) callbacks_.onChange();
    };

    ShareCenter::Hooks hooks;
    hooks.setRole = [this](GroupSession::Role role) { setRole(role); };
    hooks.isLoadingSource = [this] { return isLoadingSource_.load(); };
    hooks.sourceError = [this] {
        std::lock_guard<std::mutex> lock(sourceMutex_);
        return sourceError_;
    };
    center_ = std::make_unique<ShareCenter>(*session_, std::move(options.device), std::move(hooks));
    // The center publishes its first state from its own thread as soon as it exists, which may
    // be before these are set (see the note in the report on `ShareCenter`); `refresh` below
    // publishes again once they are.
    center_->onChange = [this] {
        if (callbacks_.onChange) callbacks_.onChange();
    };
    center_->onAlert = [this](const ShareCenter::Share& share) {
        if (callbacks_.onAlert) callbacks_.onAlert(share);
    };
    center_->setAlertsEnabled(options.alertsEnabled);
    center_->refresh();
}

Engine::~Engine() {
    // The center goes first (its thread drives the session and the output), then the output,
    // then the session, whose threads call back into this object.
    center_.reset();
    output_->stop();
    session_.reset();
}

void Engine::setRole(GroupSession::Role role) {
    session_->setRole(role);
    if (role == GroupSession::Role::off) {
        output_->stop();
    } else {
        output_->start();
    }
}

std::string Engine::loadTrack(const std::string& path, const std::string& name, const uint8_t digest[32]) {
    std::string error;
    auto file = loadMediaFile(path, name, digest, error);
    if (file) session_->addTrack(std::move(file));
    return error;
}

void Engine::playSelected() {
    const GroupSession::Snapshot snapshot = session_->snapshot();
    if (snapshot.role == GroupSession::Role::leader && snapshot.selectedTrack) session_->play(snapshot.selectedTrack->id);
}

void Engine::setSourceStatus(bool isLoading, const std::string& error) {
    {
        std::lock_guard<std::mutex> lock(sourceMutex_);
        sourceError_ = error;
    }
    isLoadingSource_ = isLoading;
}

std::string Engine::stateJSON() const {
    const ShareCenter::State state = center_->state();
    JsonWriter json;
    json.beginObject();
    json.key("mode").value(state.mode == ShareCenter::Mode::share ? "share" : "listen");
    json.key("deviceName").value(state.deviceName);
    json.key("nearby").beginArray();
    for (const auto& share : state.nearby) writeShare(json, share);
    json.endArray();
    json.key("alerts").beginArray();
    for (const auto& share : state.alerts) writeShare(json, share);
    json.endArray();
    json.key("hasUnseenShares").value(state.hasUnseenShares);
    json.key("discoveryError").value(state.discoveryError);

    json.key("joined");
    if (state.joined) writeShare(json, *state.joined); else json.null();
    json.key("isPlaying").value(state.isPlaying);
    json.key("channel").value(static_cast<int>(state.channel));
    json.key("joinedLayout").value(static_cast<int>(state.joinedLayout));
    json.key("syncStatus").value(syncStatusName(state.syncStatus));

    json.key("source");
    if (state.source) {
        json.beginObject().key("name").value(state.source->name).key("kind").value(static_cast<int>(state.source->kind)).endObject();
    } else {
        json.null();
    }
    json.key("isSharing").value(state.isSharing);
    json.key("shareProblem").value(state.shareProblem);
    json.key("layout").value(static_cast<int>(state.layout));
    json.key("listeners").beginArray();
    for (const auto& listener : state.listeners) {
        json.beginObject().key("id").id(listener.id).key("name").value(listener.name).key("channel");
        if (listener.channel) json.value(static_cast<int>(*listener.channel)); else json.null();
        json.endObject();
    }
    json.endArray();
    json.key("revision").value(static_cast<int64_t>(state.revision));
    json.endObject();
    return json.text();
}

std::string shareJSON(const ShareCenter::Share& share) {
    JsonWriter json;
    writeShare(json, share);
    return json.text();
}

std::string Engine::snapshotJSON() const {
    const GroupSession::Snapshot state = session_->snapshot();
    JsonWriter json;
    json.beginObject();
    json.key("role").value(roleName(state.role));
    json.key("status").value(state.status);
    json.key("deviceID").id(state.identity.id);
    json.key("deviceName").value(state.identity.name);
    json.key("revision").value(static_cast<int64_t>(state.revision));

    json.key("connectedLeader");
    if (state.connectedLeader) json.value(*state.connectedLeader); else json.null();
    json.key("followerState");
    if (state.followerState) json.value(followerStateName(*state.followerState)); else json.null();
    json.key("isAudioConnected").value(state.isAudioConnected);
    json.key("remoteLayout").value(static_cast<int>(state.remoteLayout));
    json.key("rounds").beginArray();
    for (const auto& round : state.rounds) writeRound(json, round);
    json.endArray();
    json.key("latestRound");
    if (state.latestRound) writeRound(json, *state.latestRound); else json.null();

    json.key("leaderPort").value(static_cast<int>(state.leaderPort));
    json.key("followerCount").value(state.followerCount);
    json.key("members").beginArray();
    for (const auto& member : state.members) {
        json.beginObject().key("id").id(member.id).key("name").value(member.name).key("isActive").value(member.isActive).endObject();
    }
    json.endArray();
    json.key("layout").value(static_cast<int>(state.speakers.layout));
    json.key("assignments").beginArray();
    for (const auto& [channel, device] : state.speakers.assignments) {
        json.beginObject().key("channel").value(static_cast<int>(channel)).key("device").id(device).endObject();
    }
    json.endArray();
    json.key("activeChannels").beginArray();
    for (const auto& [device, channel] : state.activeChannels) {
        json.beginObject().key("device").id(device).key("channel").value(static_cast<int>(channel)).endObject();
    }
    json.endArray();
    json.key("isLayoutComplete").value(state.isLayoutComplete);
    json.key("isPreparingPlayback").value(state.isPreparingPlayback);
    json.key("selectedTrack");
    if (state.selectedTrack) {
        json.beginObject().key("id").id(state.selectedTrack->id).key("name").value(state.selectedTrack->name).key("duration").value(state.selectedTrack->duration).endObject();
    } else {
        json.null();
    }

    json.key("source").id(state.playback.source);
    json.key("isPlaying").value(state.playback.isPlaying());
    json.key("sourceDescription").value(session_->describe(state.playback.source));
    json.key("channel").value(static_cast<int>(state.channel));
    json.key("hasClockMap").value(state.clockMap.has_value());
    json.key("isWaitingForAudio").value(state.isWaitingForAudio);

    json.key("log").beginArray();
    for (const auto& line : state.log) {
        const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(line.date.time_since_epoch()).count();
        json.beginObject().key("date").value(static_cast<int64_t>(milliseconds)).key("text").value(line.text).endObject();
    }
    json.endArray();
    json.endObject();
    return json.text();
}

std::string Engine::liveJSON() const {
    const GroupSession::Snapshot state = session_->snapshot();
    JsonWriter json;
    json.beginObject();
    json.key("stats");
    writeStats(json, session_->renderer().latestStats());
    json.key("output");
    writeOutput(json, output_->status(), output_->manualOffset());
    writeLeaderClock(json, state.clockMap);
    json.endObject();
    return json.text();
}

}  // namespace asyncmusic::android
