//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

// Hosts a group with the core's leader on any platform, for trying followers (an Apple device,
// the Windows app) against it without a full app. Plays a WAV file (16/24/32-bit PCM or float),
// or the metronome without one. It does not advertise itself; register it with the system's
// mDNS tool, e.g. on macOS:
//
//     test_host music.wav &
//     dns-sd -R "Test Host" _asyncmusic._udp local <the port it prints>
//
// It starts playing once a follower joins, and plays nothing itself.
//
// Usage: test_host [file.wav] [--stereo] [--seconds N]

#include <asyncmusic/host_clock.hpp>
#include <asyncmusic/media.hpp>
#include <asyncmusic/session.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

using namespace asyncmusic;

namespace {

/// Just enough of a WAV reader for testing: the whole file in memory.
struct Wave {
    double rate = 0;
    int channels = 0;
    std::vector<float> samples;  // interleaved

    int64_t frames() const { return channels > 0 ? static_cast<int64_t>(samples.size()) / channels : 0; }

    static bool load(const char* path, Wave& wave) {
        std::ifstream file(path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const auto u16 = [&](size_t at) { return static_cast<uint32_t>(bytes[at] | bytes[at + 1] << 8); };
        const auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
        if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;
        int format = 0, bits = 0;
        for (size_t at = 12; at + 8 <= bytes.size();) {
            const uint32_t size = u32(at + 4);
            const size_t body = at + 8;
            if (body + size > bytes.size()) break;
            if (std::memcmp(bytes.data() + at, "fmt ", 4) == 0) {
                format = static_cast<int>(u16(body));
                wave.channels = static_cast<int>(u16(body + 2));
                wave.rate = u32(body + 4);
                bits = static_cast<int>(u16(body + 14));
                if (format == 0xFFFE) format = static_cast<int>(u16(body + 24));  // extensible
            } else if (std::memcmp(bytes.data() + at, "data", 4) == 0 && wave.channels > 0) {
                const int width = bits / 8;
                for (size_t i = body; i + width <= body + size; i += static_cast<size_t>(width)) {
                    float value = 0;
                    if (format == 3 && bits == 32) std::memcpy(&value, &bytes[i], 4);
                    else if (bits == 16) value = static_cast<int16_t>(u16(i)) / 32768.0f;
                    else if (bits == 24) value = static_cast<float>(static_cast<int32_t>(u32(i - 1) & 0xFFFFFF00u)) / 2147483648.0f;
                    else if (bits == 32) value = static_cast<float>(static_cast<int32_t>(u32(i))) / 2147483648.0f;
                    wave.samples.push_back(value);
                }
            }
            at = body + size + (size & 1);
        }
        return wave.channels > 0 && wave.rate > 0 && !wave.samples.empty();
    }
};

class WaveDecoder final : public AudioDecoder {
public:
    explicit WaveDecoder(std::shared_ptr<const Wave> wave) : wave_(std::move(wave)) {}
    bool seek(int64_t frame) override {
        position_ = std::min(frame, wave_->frames());
        return true;
    }
    int read(float* left, float* right, int capacity) override {
        const int count = static_cast<int>(std::min<int64_t>(capacity, wave_->frames() - position_));
        const int channels = wave_->channels;
        for (int i = 0; i < count; ++i) {
            const size_t at = static_cast<size_t>((position_ + i) * channels);
            left[i] = wave_->samples[at];
            right[i] = channels > 1 ? wave_->samples[at + 1] : left[i];
        }
        position_ += count;
        return count;
    }

private:
    std::shared_ptr<const Wave> wave_;
    int64_t position_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    bool stereo = false;
    double seconds = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stereo") == 0) stereo = true;
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = std::atof(argv[++i]);
        else path = argv[i];
    }
    std::shared_ptr<MediaFile> file;
    if (path != nullptr) {
        auto wave = std::make_shared<Wave>();
        if (!Wave::load(path, *wave)) {
            std::printf("Not a WAV file I can read: %s\n", path);
            return 1;
        }
        file = std::make_shared<MediaFile>();
        file->id = kPlaybackSourceFirstFile + std::random_device()();
        file->name = "Test Host Audio";
        file->sampleRate = wave->rate;
        file->frameCount = wave->frames();
        file->openDecoder = [wave] { return std::make_unique<WaveDecoder>(wave); };
    }

    GroupSession session(DeviceIdentity{0x7E57'0000'0000'0000ull | std::random_device()(), "Test Host"});
    session.setRole(GroupSession::Role::leader);
    session.setSpeakerLayout(stereo ? SpeakerLayout::stereoPair : SpeakerLayout::everywhere);
    if (file) session.addTrack(file);
    while (session.snapshot().leaderPort == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::printf("Leading on UDP port %u\n", session.snapshot().leaderPort);
    std::fflush(stdout);

    bool started = false;
    size_t printed = 0;
    const auto start = std::chrono::steady_clock::now();
    while (seconds <= 0 || std::chrono::steady_clock::now() - start < std::chrono::duration<double>(seconds)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto state = session.snapshot();
        if (!started && !state.members.empty()) {
            started = true;
            session.play(file ? file->id : kPlaybackSourceMetronome);
        }
        if (state.log.size() < printed) printed = 0;
        for (; printed < state.log.size(); ++printed) std::printf("  | %s\n", state.log[printed].text.c_str());
        std::string members;
        for (const auto& member : state.members) members += member.name + (member.isActive ? "" : " (silent)") + "; ";
        const RenderStats stats = session.renderer().latestStats();
        std::printf("%s | %s | media %.1fs | members: %s\n", state.status.c_str(), session.describe(state.playback.source).c_str(), stats.mediaSeconds,
            members.empty() ? "none" : members.c_str());
        std::fflush(stdout);
    }
}
