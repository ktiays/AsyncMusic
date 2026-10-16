//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/session.hpp>

#include <functional>
#include <memory>
#include <string>

#include "AudioOutput.h"
#include "LeaderBrowser.h"

namespace asyncmusic::windows {

/// Everything the Windows app runs: the group session, the audio output playing it and the
/// search for leaders, wired together and kept in step with the role.
class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /// Called from background threads whenever the session or the list of leaders changed.
    std::function<void()> onChange;

    GroupSession& session() { return *session_; }
    const AudioOutput& output() const { return *output_; }
    const LeaderBrowser& browser() const { return *browser_; }

    /// Audio and browsing run only while part of a group.
    void setRole(GroupSession::Role role);
    void follow(const LeaderBrowser::Leader& leader);

    double manualOffset() const { return output_->manualOffset(); }
    void setManualOffset(double milliseconds);

    /// Pass an empty name to go back to the computer's name.
    void rename(const std::string& name);

private:
    std::unique_ptr<GroupSession> session_;
    std::unique_ptr<AudioOutput> output_;
    std::unique_ptr<LeaderBrowser> browser_;
};

}  // namespace asyncmusic::windows
