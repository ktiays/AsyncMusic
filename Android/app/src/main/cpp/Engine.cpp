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

}  // namespace

Engine::Engine(DeviceIdentity identity, GroupSession::SpeakerSettings speakers, double manualOffset, Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {
    session_ = std::make_unique<GroupSession>(std::move(identity), std::move(speakers));
    output_ = std::make_unique<AudioOutput>(session_->renderer());
    output_->setManualOffset(manualOffset);
    session_->onSpeakerSettingsChange = [this](const GroupSession::SpeakerSettings& settings) {
        if (callbacks_.onSpeakerSettingsChange) callbacks_.onSpeakerSettingsChange(settings);
    };
    session_->onChange = [this] {
        if (callbacks_.onChange) callbacks_.onChange();
    };
}

Engine::~Engine() {
    output_->stop();
    // The session goes first: its threads call back into this object.
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

void Engine::follow(const std::string& leader) {
    session_->follow(leader, [this, leader]() -> std::optional<net::Address> {
        return callbacks_.resolve ? callbacks_.resolve(leader) : std::nullopt;
    });
}

std::string Engine::loadTrack(const std::string& path, const std::string& name, const uint8_t digest[32]) {
    std::string error;
    auto file = loadMediaFile(path, name, digest, error);
    if (file) session_->addTrack(std::move(file));
    return error;
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
