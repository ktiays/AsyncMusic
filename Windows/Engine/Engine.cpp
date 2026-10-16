//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "Engine.h"

#include "Settings.h"

namespace asyncmusic::windows {

Engine::Engine() {
    session_ = std::make_unique<GroupSession>(settings::deviceIdentity());
    output_ = std::make_unique<AudioOutput>(session_->renderer());
    browser_ = std::make_unique<LeaderBrowser>();
    output_->setManualOffset(settings::manualOffset());
    session_->onChange = [this] {
        if (onChange) onChange();
    };
    browser_->onChange = [this] {
        if (onChange) onChange();
    };
}

Engine::~Engine() {
    browser_->stop();
    output_->stop();
    // The session goes first: its threads call back into this object.
    session_.reset();
}

void Engine::setRole(GroupSession::Role role) {
    session_->setRole(role);
    if (role == GroupSession::Role::off) {
        browser_->stop();
        output_->stop();
    } else {
        output_->start();
        browser_->start();
    }
    if (onChange) onChange();
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
