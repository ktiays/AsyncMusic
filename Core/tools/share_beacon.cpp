//
//  Created by ktiays on 2026/11/05.
//  Copyright (c) 2026 ktiays. All rights reserved.
//
// Shares or listens with the core's `ShareDiscovery` on the real network, for checking that
// share announcements get between devices (and how many are lost) without the apps.
//
//     share_beacon listen [--seconds N]
//     share_beacon share <name> [--source S] [--clock-port P] [--update-every S] [--seconds N]
//
// A listener prints every share that appears, changes or goes away, and every five seconds how
// many announcements of each arrived and how many were lost. A sharer prints what it sent and
// where; with `--update-every` it changes its listener count that often (to watch updates
// arrive). Ctrl-C stops either; a sharer says goodbye on the way out.

#include <asyncmusic/share_discovery.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>

using namespace asyncmusic;

namespace {

std::atomic<bool> interrupted{false};

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

const char* describe(ShareEvent::Kind kind) {
    switch (kind) {
    case ShareEvent::Kind::appeared: return "APPEARED";
    case ShareEvent::Kind::updated: return "updated";
    case ShareEvent::Kind::disappeared: return "GONE";
    }
    return "?";
}

const char* describe(ShareEvent::Reason reason) {
    switch (reason) {
    case ShareEvent::Reason::none: return "";
    case ShareEvent::Reason::ended: return " (said goodbye)";
    case ShareEvent::Reason::timedOut: return " (timed out)";
    case ShareEvent::Reason::replaced: return " (replaced by a new share)";
    }
    return "";
}

void printStatus(const ShareDiscovery::Status& status) {
    std::string targets;
    for (const auto& target : status.targets) targets += (targets.empty() ? "" : ", ") + target;
    std::printf("  socket %s%s%s | broadcasting to %s\n", status.isReady ? "ready" : "NOT READY", status.error.empty() ? "" : ": ", status.error.c_str(),
        targets.empty() ? "255.255.255.255 (no interface found)" : targets.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || (std::strcmp(argv[1], "listen") != 0 && std::strcmp(argv[1], "share") != 0) || (std::strcmp(argv[1], "share") == 0 && argc < 3)) {
        std::printf("Usage: share_beacon listen [--seconds N]\n"
                    "       share_beacon share <name> [--source S] [--model M] [--device laptop|desktop|phone|tablet]\n"
                    "                          [--clock-port P] [--update-every S] [--seconds N]\n");
        return 1;
    }
    const bool sharing = std::strcmp(argv[1], "share") == 0;
    ShareInfo info;
    info.sourceKind = ShareSourceKind::application;
    info.source = "Test Tone";
    info.clockPort = 50000;
    double seconds = 0;
    double updateEvery = 0;
    for (int i = sharing ? 3 : 2; i < argc; ++i) {
        const auto has = [&](const char* flag) { return std::strcmp(argv[i], flag) == 0 && i + 1 < argc; };
        if (has("--seconds")) seconds = std::atof(argv[++i]);
        else if (has("--source")) info.source = argv[++i];
        else if (has("--model")) info.model = argv[++i];
        else if (has("--device")) {
            const std::string kind = argv[++i];
            info.deviceKind = kind == "laptop" ? ShareDeviceKind::laptop
                : kind == "desktop"            ? ShareDeviceKind::desktop
                : kind == "phone"              ? ShareDeviceKind::phone
                : kind == "tablet"             ? ShareDeviceKind::tablet
                                               : ShareDeviceKind::unknown;
        }
        else if (has("--clock-port")) info.clockPort = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (has("--update-every")) updateEvery = std::atof(argv[++i]);
    }
    if (sharing) info.name = argv[2];

    std::signal(SIGINT, [](int) { interrupted = true; });
    const auto start = std::chrono::steady_clock::now();
    const uint64_t device = (static_cast<uint64_t>(std::random_device()()) << 32) | std::random_device()();

    ShareDiscovery discovery(device, [start](const ShareEvent& event) {
        const auto& share = event.share;
        std::printf("[%7.2f] %s %s%s — %s, %s, %u listening, rev %u, share %016llx\n", secondsSince(start), describe(event.kind), share.info.name.c_str(),
            describe(event.reason), share.address.toString().c_str(), share.info.source.c_str(), share.info.listenerCount, share.revision,
            static_cast<unsigned long long>(share.share));
        std::fflush(stdout);
    });

    if (sharing) {
        discovery.share(info);
        std::printf("Sharing as \"%s\" (device %016llx), clock port %u\n", info.name.c_str(), static_cast<unsigned long long>(device), info.clockPort);
    } else {
        discovery.listen();
        std::printf("Listening on port %u (device %016llx)\n", kShareDiscoveryPort, static_cast<unsigned long long>(device));
    }
    std::fflush(stdout);

    auto nextReport = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto nextUpdate = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(updateEvery));
    while (!interrupted && (seconds <= 0 || secondsSince(start) < seconds)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (sharing && updateEvery > 0 && now >= nextUpdate) {
            ++info.listenerCount;
            discovery.update(info);
            nextUpdate = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(updateEvery));
            std::printf("[%7.2f] now %u listening\n", secondsSince(start), info.listenerCount);
        }
        if (now < nextReport) continue;
        nextReport = now + std::chrono::seconds(5);
        const auto status = discovery.status();
        std::printf("[%7.2f] status\n", secondsSince(start));
        printStatus(status);
        if (sharing) {
            std::printf("  sent %u announcements, answered %u queries\n", status.announcementsSent, status.queriesAnswered);
        } else {
            const auto shares = discovery.shares();
            if (shares.empty()) std::printf("  no shares heard\n");
            for (const auto& share : shares) {
                const uint32_t expected = share.received + share.missed;
                const int64_t nowNanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                std::printf("  %-24s %-22s received %4u, lost %3u (%.1f%%), last heard %.1fs ago\n", share.info.name.c_str(), share.address.toString().c_str(),
                    share.received, share.missed, expected > 0 ? 100.0 * share.missed / expected : 0.0, static_cast<double>(nowNanoseconds - share.lastSeen) / 1e9);
            }
        }
        std::fflush(stdout);
    }
    discovery.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::printf("Stopped.\n");
}
