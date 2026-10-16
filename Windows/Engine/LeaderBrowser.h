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

}  // namespace asyncmusic::windows
