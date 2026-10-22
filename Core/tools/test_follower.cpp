//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//
// Follows a leader at a given address with the core's `GroupSession`, for trying hosts (an
// Android device, the Windows app) against it without a full app. It plays nothing: a thread
// stands in for an audio output, rendering 10 ms buffers on the host clock and throwing them
// away, so the renderer's statistics (schedule error, buffered audio, level) show whether the
// host's clock and stream arrive. Reaching a host that mDNS cannot (e.g. an Android emulator,
// through `adb emu redir add udp:<port>:<port>` and the same for the stream's TCP port):
//
//     test_follower 127.0.0.1 <port>
//
// Usage: test_follower <host> <port> [--seconds N]

#include <asyncmusic/host_clock.hpp>
#include <asyncmusic/session.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace asyncmusic;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("Usage: test_follower <host> <port> [--seconds N]\n");
        return 1;
    }
    const auto address = net::Address::fromString(argv[1], static_cast<uint16_t>(std::atoi(argv[2])));
    if (!address) {
        std::printf("Not a numeric address: %s\n", argv[1]);
        return 1;
    }
    double seconds = 0;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atof(argv[++i]);
    }

    GroupSession session(DeviceIdentity{0x7E57'0000'0000'0000ull | std::random_device()(), "Test Follower"});
    session.setRole(GroupSession::Role::follower);
    session.follow(address->toString(), [address] { return std::optional<net::Address>(*address); });

    // The stand-in for an audio output: 10 ms buffers at 48 kHz, presented as they are rendered.
    std::atomic<bool> running{true};
    std::thread output([&] {
        constexpr int kFrames = 480;
        std::vector<float> left(kFrames), right(kFrames);
        int64_t next = hostClockNanoseconds();
        while (running) {
            session.renderer().render(true, next, kFrames, 48000, left.data(), right.data());
            next += 10'000'000;
            std::this_thread::sleep_until(std::chrono::steady_clock::now() + std::chrono::nanoseconds(next - hostClockNanoseconds()));
        }
    });

    size_t printed = 0;
    const auto start = std::chrono::steady_clock::now();
    while (seconds <= 0 || std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto state = session.snapshot();
        if (state.log.size() < printed) printed = 0;
        for (; printed < state.log.size(); ++printed) std::printf("  | %s\n", state.log[printed].text.c_str());
        const RenderStats stats = session.renderer().latestStats();
        const auto& round = state.latestRound;
        std::printf("%s | %s%s | channel %d | rtt %.2f ms, fit %.0f µs | media %.2fs, error %+.0f µs, buffered %.1fs, gaps %d, peak %.3f\n",
            state.status.c_str(), session.describe(state.playback.source).c_str(), state.isWaitingForAudio ? " (waiting for audio)" : "",
            static_cast<int>(state.channel), round ? static_cast<double>(round->minimumRoundTrip) / 1e6 : 0.0, round ? round->residualRMS / 1e3 : 0.0,
            stats.mediaSeconds, stats.errorMicroseconds, stats.bufferedSeconds, stats.gaps, stats.outputPeak);
        std::fflush(stdout);
    }
    running = false;
    output.join();
}
