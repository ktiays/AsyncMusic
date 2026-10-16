//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

// Runs the Windows engine without UI, for testing over ssh:
//
//     asyncmusic-cli list [seconds]
//     asyncmusic-cli join <leader name | ip:port> [--seconds N] [--offset ms] [--no-audio]
//     asyncmusic-cli host [audio file] [--seconds N] [--stereo] [--channel-test]

#include <windows.h>

#include <asyncmusic/host_clock.hpp>
#include <asyncmusic/session.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#include "AudioOutput.h"
#include "Engine.h"
#include "LeaderBrowser.h"
#include "Settings.h"
#include "Text.h"

using namespace asyncmusic;
using namespace asyncmusic::windows;

namespace {

void sleepSeconds(double seconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

int list(double seconds) {
    LeaderBrowser browser;
    if (!browser.start()) {
        std::printf("%s\n", browser.error().c_str());
        return 1;
    }
    sleepSeconds(seconds);
    for (const auto& leader : browser.leaders()) {
        const auto address = LeaderBrowser::resolve(leader.instance);
        std::printf("%s\t%s\n", leader.name.c_str(), address ? address->toString().c_str() : "(unresolved)");
    }
    return 0;
}

/// Finds a leader by name (or takes an address) and returns how to look it up again.
ClockSyncFollower::Resolver makeResolver(const std::string& target) {
    const auto colon = target.rfind(':');
    if (colon != std::string::npos) {
        if (auto address = net::Address::fromString(target.substr(0, colon), static_cast<uint16_t>(std::stoi(target.substr(colon + 1))))) {
            return [address] { return address; };
        }
    }
    auto browser = std::make_shared<LeaderBrowser>();
    browser->start();
    return [browser, target]() -> std::optional<net::Address> {
        for (int attempt = 0; attempt < 30; ++attempt) {
            for (const auto& leader : browser->leaders()) {
                if (leader.name == target) return LeaderBrowser::resolve(leader.instance);
            }
            sleepSeconds(0.1);
        }
        return std::nullopt;
    };
}

int join(const std::string& target, double seconds, double offset, bool audio) {
    GroupSession session(settings::deviceIdentity());
    AudioOutput output(session.renderer());
    output.setManualOffset(offset);
    if (audio) output.start();
    session.setRole(GroupSession::Role::follower);
    session.follow(target, makeResolver(target));

    size_t printedLog = 0;
    const auto start = std::chrono::steady_clock::now();
    while (seconds <= 0 || std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds)) {
        sleepSeconds(1);
        const GroupSession::Snapshot state = session.snapshot();
        // New log lines (the log keeps the most recent 200).
        if (state.log.size() < printedLog) printedLog = 0;
        for (size_t i = printedLog; i < state.log.size(); ++i) {
            std::printf("  | %s\n", state.log[i].text.c_str());
        }
        printedLog = state.log.size();

        const RenderStats stats = session.renderer().latestStats();
        const AudioOutput::Status device = output.status();
        std::printf("%s | %s | err %+.0fus corr %+.0fppm jumps %d | media %.2fs buffered %.2fs gaps %d peak %.2f | %s %.0fHz buf %.1fms jitter %.0fus%s\n",
            state.status.c_str(), session.describe(state.playback.source).c_str(), stats.errorMicroseconds, stats.correctionPPM, stats.jumps,
            stats.mediaSeconds, stats.bufferedSeconds, stats.gaps, stats.outputPeak, device.deviceName.c_str(), device.sampleRate,
            device.bufferDuration * 1e3, device.timestampJitter, device.error.empty() ? "" : (" ERROR " + device.error).c_str());
        std::fflush(stdout);
    }
    output.stop();
    return 0;
}

/// Leads a group from here: plays `file` (or the metronome, or the channel test) on every
/// follower that joins, and reports the group every second.
int host(const std::wstring& file, double seconds, bool stereo, bool channelTest) {
    Engine engine;
    GroupSession& session = engine.session();
    engine.setRole(GroupSession::Role::leader);
    session.setSpeakerLayout(stereo ? SpeakerLayout::stereoPair : SpeakerLayout::everywhere);
    if (!file.empty()) {
        engine.loadTrack(file);
        while (engine.isLoadingTrack()) sleepSeconds(0.1);
        const auto track = session.snapshot().selectedTrack;
        if (!track) {
            std::printf("Could not load the file: %s\n", engine.loadError().c_str());
            return 1;
        }
    }
    bool started = false;
    size_t printedLog = 0;
    const auto start = std::chrono::steady_clock::now();
    while (seconds <= 0 || std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds)) {
        sleepSeconds(1);
        const GroupSession::Snapshot state = session.snapshot();
        // Start once someone joined (or after a few seconds alone).
        if (!started && (!state.members.empty() || std::chrono::steady_clock::now() - start > std::chrono::seconds(8))) {
            started = true;
            session.play(channelTest ? kPlaybackSourceChannelTest : state.selectedTrack ? state.selectedTrack->id : kPlaybackSourceMetronome);
        }
        if (state.log.size() < printedLog) printedLog = 0;
        for (size_t i = printedLog; i < state.log.size(); ++i) std::printf("  | %s\n", state.log[i].text.c_str());
        printedLog = state.log.size();

        std::string members;
        for (const auto& member : state.members) {
            const auto channel = state.activeChannels.find(member.id);
            members += member.name + (member.isActive ? "" : " (silent)")
                + (channel == state.activeChannels.end() ? "" : channel->second == SpeakerChannel::left ? " [L]" : " [R]") + "; ";
        }
        const RenderStats stats = session.renderer().latestStats();
        const std::string advertising = engine.advertisingError();
        std::printf("%s | port %u | %s | %s | err %+.0fus media %.2fs peak %.2f | members: %s%s\n", state.status.c_str(), state.leaderPort,
            session.describe(state.playback.source).c_str(), state.isPreparingPlayback ? "preparing" : "-", stats.errorMicroseconds, stats.mediaSeconds,
            stats.outputPeak, members.empty() ? "none" : members.c_str(), advertising.empty() ? "" : (" | mDNS: " + advertising).c_str());
        std::fflush(stdout);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    if (argc >= 2 && std::strcmp(argv[1], "list") == 0) {
        return list(argc >= 3 ? std::atof(argv[2]) : 3);
    }
    if (argc >= 3 && std::strcmp(argv[1], "join") == 0) {
        double seconds = 0;
        double offset = settings::manualOffset();
        bool audio = true;
        for (int i = 3; i < argc; ++i) {
            if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atof(argv[++i]);
            else if (std::strcmp(argv[i], "--offset") == 0 && i + 1 < argc) offset = std::atof(argv[++i]);
            else if (std::strcmp(argv[i], "--no-audio") == 0) audio = false;
        }
        return join(argv[2], seconds, offset, audio);
    }
    if (argc >= 2 && std::strcmp(argv[1], "host") == 0) {
        std::wstring file;
        double seconds = 0;
        bool stereo = false;
        bool channelTest = false;
        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atof(argv[++i]);
            else if (std::strcmp(argv[i], "--stereo") == 0) stereo = true;
            else if (std::strcmp(argv[i], "--channel-test") == 0) channelTest = true;
            else file = toUTF16(argv[i]);
        }
        return host(file, seconds, stereo, channelTest);
    }
    std::printf("usage:\n  asyncmusic-cli list [seconds]\n  asyncmusic-cli join <leader name | ip:port> [--seconds N] [--offset ms] [--no-audio]\n"
                "  asyncmusic-cli host [audio file] [--seconds N] [--stereo] [--channel-test]\n");
    return 2;
}
