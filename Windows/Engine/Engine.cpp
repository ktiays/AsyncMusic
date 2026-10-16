//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "Engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Settings.h"

namespace asyncmusic::windows {

Engine::Engine() {
    session_ = std::make_unique<GroupSession>(settings::deviceIdentity(), settings::speakerSettings());
    output_ = std::make_unique<AudioOutput>(session_->renderer());
    browser_ = std::make_unique<LeaderBrowser>();
    advertiser_ = std::make_unique<LeaderAdvertiser>();
    output_->setManualOffset(settings::manualOffset());
    session_->onSpeakerSettingsChange = [](const GroupSession::SpeakerSettings& speakers) { settings::setSpeakerSettings(speakers); };
    session_->onChange = [this] {
        advertising_.async([this] { updateAdvertisement(); });
        if (onChange) onChange();
    };
    browser_->onChange = [this] {
        if (onChange) onChange();
    };
}

Engine::~Engine() {
    stayAwake(false);
    if (powerRequest_ != nullptr) CloseHandle(powerRequest_);
    loading_.shutdown();
    browser_->stop();
    output_->stop();
    // The session goes first: its threads call back into this object.
    session_.reset();
    advertising_.shutdown();
    advertiser_->stop();
}

void Engine::setRole(GroupSession::Role role) {
    session_->setRole(role);
    stayAwake(role != GroupSession::Role::off);
    if (role == GroupSession::Role::off) {
        output_->stop();
    } else {
        output_->start();
    }
    if (role == GroupSession::Role::follower) {
        browser_->start();
    } else {
        browser_->stop();
    }
    if (onChange) onChange();
}

void Engine::stayAwake(bool awake) {
    if (powerRequest_ == nullptr) {
        REASON_CONTEXT reason{};
        reason.Version = POWER_REQUEST_CONTEXT_VERSION;
        reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        reason.Reason.SimpleReasonString = const_cast<LPWSTR>(L"Playing music in sync with other devices");
        powerRequest_ = PowerCreateRequest(&reason);
        if (powerRequest_ == INVALID_HANDLE_VALUE) powerRequest_ = nullptr;
    }
    if (powerRequest_ == nullptr) return;
    if (awake) {
        PowerSetRequest(powerRequest_, PowerRequestSystemRequired);
    } else {
        PowerClearRequest(powerRequest_, PowerRequestSystemRequired);
    }
}

/// Advertises the session's clock port under this device's name while leading.
void Engine::updateAdvertisement() {
    const GroupSession::Snapshot state = session_->snapshot();
    if (state.role == GroupSession::Role::leader && state.leaderPort != 0) {
        advertiser_->advertise(state.identity.name, state.leaderPort);
    } else {
        advertiser_->stop();
    }
}

std::string Engine::advertisingError() const {
    return advertiser_->error();
}

void Engine::loadTrack(const std::wstring& path) {
    ++loadingTracks_;
    if (onChange) onChange();
    loading_.async([this, path] {
        std::string error;
        auto file = loadMediaFile(path, error);
        if (file) session_->addTrack(std::move(file));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            loadError_ = error;
        }
        --loadingTracks_;
        if (onChange) onChange();
    });
}

std::string Engine::loadError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return loadError_;
}

void Engine::follow(const LeaderBrowser::Leader& leader) {
    const std::wstring instance = leader.instance;
    session_->follow(leader.name, [instance] { return LeaderBrowser::resolve(instance); });
}

void Engine::setManualOffset(double milliseconds) {
    output_->setManualOffset(milliseconds);
    settings::setManualOffset(milliseconds);
}

void Engine::rename(const std::string& name) {
    settings::setDeviceName(name);
    session_->setIdentity(settings::deviceIdentity());
}

}  // namespace asyncmusic::windows
