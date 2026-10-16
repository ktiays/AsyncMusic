//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/net.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace asyncmusic::windows {

/// Finds leaders on the local network through the system's mDNS (DNS-SD) service:
/// instances of `_asyncmusic._udp`, which leaders register under their device name.
class LeaderBrowser {
public:
    struct Leader {
        /// The instance name, which is the leader's device name (UTF-8).
        std::string name;
        /// The full service instance name, for resolving.
        std::wstring instance;
    };

    LeaderBrowser();
    ~LeaderBrowser();
    LeaderBrowser(const LeaderBrowser&) = delete;
    LeaderBrowser& operator=(const LeaderBrowser&) = delete;

    /// Called (on a system thread) whenever the list changes.
    std::function<void()> onChange;

    bool start();
    void stop();
    bool isBrowsing() const;
    std::string error() const;

    /// Current leaders, sorted by name.
    std::vector<Leader> leaders() const;

    /// Looks up where `instance` listens now. Blocks for up to `timeoutMilliseconds`.
    static std::optional<net::Address> resolve(const std::wstring& instance, int timeoutMilliseconds = 3000);

private:
    struct Browse;
    void handle(unsigned long status, void* records);

    mutable std::mutex mutex_;
    std::map<std::wstring, Leader> leaders_;
    Browse* browse_ = nullptr;
    std::string error_;
};

/// Advertises this device as a leader through the system's mDNS: the instance `name` of
/// `_asyncmusic._udp` at this computer's `.local` host name, on the clock sync port.
class LeaderAdvertiser {
public:
    LeaderAdvertiser() = default;
    ~LeaderAdvertiser();
    LeaderAdvertiser(const LeaderAdvertiser&) = delete;
    LeaderAdvertiser& operator=(const LeaderAdvertiser&) = delete;

    /// Registers (or re-registers, if the name or port changed) the service.
    bool advertise(const std::string& name, uint16_t port);
    /// Withdraws the registration, waiting briefly for the system to confirm.
    void stop();
    std::string error() const;

private:
    struct Registration;
    mutable std::mutex mutex_;
    Registration* registration_ = nullptr;
    std::string name_;
    uint16_t port_ = 0;
    std::string error_;
};

}  // namespace asyncmusic::windows
