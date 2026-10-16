//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/session.hpp>

#include <asyncmusic/serial_queue.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "AudioOutput.h"
#include "LeaderBrowser.h"
#include "MediaLoader.h"

namespace asyncmusic::windows {

/// Everything the Windows app runs: the group session, the audio output playing it, the search
/// for leaders and, when hosting, this device's advertisement, wired together and kept in step
/// with the role.
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

    /// Loads a file in the background and, once read, selects it for playing.
    void loadTrack(const std::wstring& path);
    bool isLoadingTrack() const { return loadingTracks_.load() > 0; }
    /// Why the last file could not be loaded (empty if it could).
    std::string loadError() const;
    /// Why this device cannot be found by others (empty while advertising or not hosting).
    std::string advertisingError() const;

    double manualOffset() const { return output_->manualOffset(); }
    void setManualOffset(double milliseconds);

    /// Pass an empty name to go back to the computer's name.
    void rename(const std::string& name);

private:
    void updateAdvertisement();
    /// Keeps the computer from sleeping while it is part of a group: the clock sync and the
    /// music would stop, and the group lose this device.
    void stayAwake(bool awake);

    MediaFoundationScope mediaFoundation_;
    std::unique_ptr<GroupSession> session_;
    std::unique_ptr<AudioOutput> output_;
    std::unique_ptr<LeaderBrowser> browser_;
    std::unique_ptr<LeaderAdvertiser> advertiser_;
    void* powerRequest_ = nullptr;
    /// Registering with mDNS and loading files can take a while; neither may hold up the
    /// session's thread or the UI.
    SerialQueue advertising_{"asyncmusic.advertising"};
    SerialQueue loading_{"asyncmusic.loading"};
    std::atomic<int> loadingTracks_{0};
    mutable std::mutex mutex_;
    std::string loadError_;
};

}  // namespace asyncmusic::windows
