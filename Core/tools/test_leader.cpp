//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

// A bare-bones leader for testing followers on other devices: answers clock probes and tells
// every follower to play the metronome (or the channel test), on any platform. It does not
// advertise itself; register it with the system's mDNS tool, e.g. on macOS:
//
//     test_leader 50000 &
//     dns-sd -R "Test Leader" _asyncmusic._udp local 50000
//
// Usage: test_leader [port] [metronome|channel-test] [full|left|right]

#include <asyncmusic/host_clock.hpp>
#include <asyncmusic/net.hpp>
#include <asyncmusic/protocol.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <string>

using namespace asyncmusic;

int main(int argc, char** argv) {
    const auto port = static_cast<uint16_t>(argc > 1 ? std::atoi(argv[1]) : 0);
    const uint64_t source = argc > 2 && std::strcmp(argv[2], "channel-test") == 0 ? kPlaybackSourceChannelTest : kPlaybackSourceMetronome;
    SpeakerChannel channel = SpeakerChannel::full;
    if (argc > 3 && std::strcmp(argv[3], "left") == 0) channel = SpeakerChannel::left;
    if (argc > 3 && std::strcmp(argv[3], "right") == 0) channel = SpeakerChannel::right;

    net::UDPSocket socket;
    if (!socket.open(net::Address::fromString("0.0.0.0", 0)->family()) || !socket.bind(port)) {
        std::printf("Could not listen: %s\n", net::lastErrorDescription().c_str());
        return 1;
    }
    std::printf("Leading on UDP port %u\n", socket.localAddress().port());
    std::fflush(stdout);

    const uint64_t session = std::random_device()() | uint64_t{1} << 40;
    const PlaybackState playback{source, hostClockNanoseconds() + 3'000'000'000, 0};
    const SpeakerLayout layout = channel == SpeakerChannel::full ? SpeakerLayout::everywhere : SpeakerLayout::stereoPair;
    std::map<std::string, std::pair<net::Address, std::string>> followers;

    const auto send = [&](const ClockSyncMessage& message, const net::Address& to) {
        const Bytes bytes = message.encode();
        socket.sendTo(bytes.data(), bytes.size(), to);
    };
    const auto sendState = [&](const net::Address& to) {
        send(ClockSyncMessage::makePlayback(1, session, playback), to);
        send(ClockSyncMessage::makeSpeakers(1, session, layout, channel, playback.startLeaderTime), to);
    };

    auto lastBroadcast = std::chrono::steady_clock::now();
    uint8_t buffer[512];
    while (true) {
        net::Address from;
        const int size = socket.receive(buffer, sizeof(buffer), 100, &from);
        const int64_t t1 = hostClockNanoseconds();
        ClockSyncMessage message;
        if (size > 0 && ClockSyncMessage::decode(buffer, static_cast<size_t>(size), message)) {
            if (message.type == ClockSyncMessage::Type::ping) {
                send(ClockSyncMessage::makePong(message.sequence, session, message.t0, t1, hostClockNanoseconds()), from);
            } else if (message.type == ClockSyncMessage::Type::hello) {
                const std::string name(message.name.begin(), message.name.end());
                if (followers.find(from.toString()) == followers.end()) {
                    std::printf("Follower %s (%016llx) at %s\n", name.c_str(), static_cast<unsigned long long>(message.device), from.toString().c_str());
                    std::fflush(stdout);
                }
                followers[from.toString()] = {from, name};
                sendState(from);
            }
        }
        if (std::chrono::steady_clock::now() - lastBroadcast > std::chrono::seconds(1)) {
            lastBroadcast = std::chrono::steady_clock::now();
            for (const auto& [key, follower] : followers) sendState(follower.first);
        }
    }
}
