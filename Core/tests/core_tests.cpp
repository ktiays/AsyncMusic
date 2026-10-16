//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

// Self-contained tests for the core; no framework needed so they build anywhere CMake does.

#include <asyncmusic/alac.hpp>
#include <asyncmusic/asyncmusic.hpp>
#include <asyncmusic/audio_stream.hpp>
#include <asyncmusic/clock_sync.hpp>
#include <asyncmusic/net.hpp>
#include <asyncmusic/serial_queue.hpp>
#include <asyncmusic/session.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace asyncmusic;

namespace {

int failures = 0;

#define CHECK(condition, ...)                                                   \
    do {                                                                        \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            std::printf("  FAILED %s:%d: %s ", __FILE__, __LINE__, #condition); \
            std::printf(__VA_ARGS__);                                           \
            std::printf("\n");                                                  \
        }                                                                       \
    } while (0)

void run(const char* name, const std::function<void()>& test) {
    const int before = failures;
    test();
    std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", name);
}

/// Same generator as the original Swift simulations.
struct SplitMix64 {
    uint64_t state;
    uint64_t next() {
        state += 0x9E3779B97F4A7C15ull;
        uint64_t z = state;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double uniform() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
    double exponential(double mean) { return -mean * std::log(std::max(uniform(), 1e-6)); }
};

std::string hex(const Bytes& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
    }
    return out;
}

// MARK: - Clock

void clockMapInverse() {
    const ClockMap map{5'000'000'000'000, 123'456'789'012, 47e-6};
    for (int64_t local : {int64_t{0}, int64_t{5'000'000'000'000}, int64_t{9'999'999'999'999}}) {
        const int64_t leader = map.leaderTimeFromLocal(local);
        CHECK(std::llabs(map.localTimeFromLeader(leader) - local) <= 1, "local %lld", static_cast<long long>(local));
    }
}

struct EstimatorResult {
    double skewPPM, finalErrorMicroseconds, worstErrorMicroseconds;
    int resets;
};

/// Simulates `rounds` probe rounds over a network with exponential jitter and spikes.
EstimatorResult simulateEstimator(double skewPPM, double jitterMs, double spikeChance, int jumpAtRound) {
    SplitMix64 rng{42};
    const int64_t trueOffset = 987'654'321'000;
    int64_t jump = 0;
    auto leader = [&](int64_t local) { return local + trueOffset + jump + static_cast<int64_t>(static_cast<double>(local) * skewPPM * 1e-6); };
    ClockEstimator estimator;
    int64_t local = 5'000'000'000'000;
    double worst = 0;
    int resets = 0;
    for (int round = 0; round < 300; ++round) {
        if (round == jumpAtRound) jump = 3'000'000'000;
        std::vector<ClockProbe> probes;
        for (int i = 0; i < 8; ++i) {
            local += 15'000'000;
            auto leg = [&] {
                double ms = 1.0 + rng.exponential(jitterMs);
                if (rng.uniform() < spikeChance) ms += 20 + rng.uniform() * 100;
                return static_cast<int64_t>(ms * 1e6);
            };
            const int64_t t0 = local;
            const int64_t arrive = t0 + leg();
            const int64_t t1 = leader(arrive);
            const int64_t t2 = t1 + 40'000;
            const int64_t t3 = arrive + 40'000 + leg();
            probes.push_back(ClockProbe{t0, t1, t2, t3});
        }
        if (estimator.addRound(probes.data(), probes.size()) == ClockVerdict::reset) ++resets;
        local += round < 10 ? 300'000'000 : 1'000'000'000;
        // Ignore the rounds right after the jump, before the estimator has noticed it.
        const bool settling = jumpAtRound >= 0 && round >= jumpAtRound && round < jumpAtRound + 15;
        if (round >= 40 && estimator.hasMap() && !settling) {
            worst = std::max(worst, std::abs(static_cast<double>(estimator.map().leaderTimeFromLocal(local) - leader(local))) / 1e3);
        }
    }
    const double finalError = static_cast<double>(estimator.map().leaderTimeFromLocal(local) - leader(local)) / 1e3;
    return {estimator.map().skewPPM(), finalError, worst, resets};
}

void estimatorCleanLink() {
    const auto r = simulateEstimator(40, 0.3, 0, -1);
    CHECK(std::abs(r.skewPPM - 40) < 1, "skew %.3f", r.skewPPM);
    CHECK(r.worstErrorMicroseconds < 60, "worst %.1fus", r.worstErrorMicroseconds);
}

void estimatorNoisyWiFi() {
    const auto r = simulateEstimator(-75, 2, 0.05, -1);
    CHECK(std::abs(r.skewPPM + 75) < 3, "skew %.3f", r.skewPPM);
    CHECK(r.worstErrorMicroseconds < 300, "worst %.1fus", r.worstErrorMicroseconds);
}

void estimatorVeryNoisy() {
    const auto r = simulateEstimator(20, 5, 0.2, -1);
    CHECK(std::abs(r.skewPPM - 20) < 4, "skew %.3f", r.skewPPM);
    CHECK(r.worstErrorMicroseconds < 700, "worst %.1fus", r.worstErrorMicroseconds);
}

void estimatorRecoversFromLeaderJump() {
    const auto r = simulateEstimator(20, 1, 0.05, 150);
    CHECK(r.resets == 1, "resets %d", r.resets);
    CHECK(std::abs(r.finalErrorMicroseconds) < 150, "final %.1fus", r.finalErrorMicroseconds);
    CHECK(r.worstErrorMicroseconds < 300, "worst outside the jump %.1fus", r.worstErrorMicroseconds);
}

// MARK: - Renderer

void rendererStartsOnTime() {
    const double rate = 48000;
    const int frames = 512;
    // One impulse at frame 0 of the stream.
    Ref<StreamBuffer> buffer = Ref<StreamBuffer>::adopt(StreamBuffer::create(rate, 48000, 0));
    std::vector<float> impulse(48000, 0.0f);
    impulse[0] = 1;
    buffer->append(impulse.data(), impulse.data(), 48000);

    const ClockMap map{0, 5'000'000'123'457, 0};
    const int64_t startLeader = 5'100'000'000'000;
    const int64_t delay = 11'650'000;
    Ref<SyncedRenderer> renderer = Ref<SyncedRenderer>::adopt(SyncedRenderer::create());
    renderer->setClockMap(map);
    renderer->setPresentationDelay(delay);
    renderer->setPlayback(PlaybackState{7, startLeader, 0}, 1, RenderSourceKind::stream, buffer.get());

    const int64_t expectedHost = map.localTimeFromLeader(startLeader) - delay;
    int64_t host = expectedHost - 300'000'000 + 7'777;
    std::vector<float> left(frames), right(frames);
    float peak = 0;
    double peakTime = 0;
    for (int k = 0; k < 60; ++k) {
        renderer->render(true, host, frames, rate, left.data(), right.data());
        for (int i = 0; i < frames; ++i) {
            if (left[static_cast<size_t>(i)] > peak) {
                peak = left[static_cast<size_t>(i)];
                peakTime = static_cast<double>(host) + i * 1e9 / rate;
            }
        }
        host += static_cast<int64_t>(frames * 1e9 / rate);
    }
    const double error = (peakTime - static_cast<double>(expectedHost)) / 1e3;
    CHECK(peak > 0.5, "peak %.3f", peak);
    CHECK(std::abs(error) < 10.5, "start error %.2fus (half a frame is 10.4us)", error);
}

void rendererTracksDrift() {
    const double rate = 48000;
    const int frames = 512;
    std::mt19937_64 random(7);
    std::uniform_int_distribution<int64_t> jitter(-50'000, 50'000);
    for (double dacPPM : {-150.0, 0.0, 250.0}) {
        Ref<SyncedRenderer> renderer = Ref<SyncedRenderer>::adopt(SyncedRenderer::create());
        ClockMap map{0, 42'000'000, 30e-6};
        renderer->setClockMap(map);
        renderer->setPlayback(PlaybackState{kPlaybackSourceMetronome, 1'000'000'000, 0}, 1, RenderSourceKind::metronome, nullptr);
        std::vector<float> left(frames), right(frames);
        double host = 0, worst = 0;
        const double hostPerBuffer = frames / (rate * (1 + dacPPM * 1e-6)) * 1e9;
        for (int k = 0; k < static_cast<int>(120 * rate) / frames; ++k) {
            if (k % 94 == 0) {
                // A new clock estimate about once a second, with +-50us of jitter.
                map.referenceOffset = 42'000'000 + jitter(random);
                renderer->setClockMap(map);
            }
            renderer->render(true, static_cast<int64_t>(host), frames, rate, left.data(), right.data());
            host += hostPerBuffer;
            if (host > 20e9) worst = std::max(worst, std::abs(renderer->latestStats().errorMicroseconds));
        }
        const RenderStats stats = renderer->latestStats();
        CHECK(stats.jumps == 0, "dac %+.0fppm jumps %d", dacPPM, stats.jumps);
        CHECK(worst < 150, "dac %+.0fppm worst error %.1fus", dacPPM, worst);
        CHECK(std::abs(stats.correctionPPM - (30 - dacPPM)) < 60, "dac %+.0fppm correction %.1fppm", dacPPM, stats.correctionPPM);
    }
}

/// A stereo pair's left device plays the left channel on both outputs, then switches to the
/// right channel exactly when the scheduled leader time is heard.
void rendererRoutesChannels() {
    const double rate = 48000;
    const int frames = 480;
    Ref<StreamBuffer> buffer = Ref<StreamBuffer>::adopt(StreamBuffer::create(rate, 10 * 48000, 0));
    std::vector<float> left(10 * 48000, 0.5f), right(10 * 48000, -0.25f);
    buffer->append(left.data(), right.data(), 10 * 48000);

    Ref<SyncedRenderer> renderer = Ref<SyncedRenderer>::adopt(SyncedRenderer::create());
    renderer->setClockMap(ClockMap{0, 0, 0});
    renderer->setPlayback(PlaybackState{20, 0, 0}, 1, RenderSourceKind::stream, buffer.get());
    renderer->setChannelMix(ChannelMix::forChannel(SpeakerChannel::left), 0);
    const int64_t switchAt = 2'000'000'000;
    renderer->setChannelMix(ChannelMix::forChannel(SpeakerChannel::right), switchAt);

    std::vector<float> outLeft(frames), outRight(frames);
    const auto cycle = static_cast<int64_t>(frames * 1e9 / rate);
    bool beforeOK = true, afterOK = true;
    int rampCycles = 0;
    for (int64_t host = 100'000'000; host < 4'000'000'000; host += cycle) {
        renderer->render(true, host, frames, rate, outLeft.data(), outRight.data());
        const float l = outLeft[frames - 1], r = outRight[frames - 1];
        if (host + cycle <= switchAt) {
            beforeOK = beforeOK && std::abs(l - 0.5f) < 1e-4f && std::abs(r - 0.5f) < 1e-4f;
        } else if (host >= switchAt) {
            afterOK = afterOK && std::abs(l + 0.25f) < 1e-4f && std::abs(r + 0.25f) < 1e-4f;
        } else {
            ++rampCycles;
        }
    }
    CHECK(beforeOK, "left channel on both outputs before the switch");
    CHECK(afterOK, "right channel on both outputs after the switch");
    CHECK(rampCycles <= 1, "switch spans %d cycles", rampCycles);

    // Full mix leaves the source untouched.
    renderer->setChannelMix(ChannelMix::forChannel(SpeakerChannel::full), 0);
    renderer->render(true, 4'000'000'000, frames, rate, outLeft.data(), outRight.data());
    renderer->render(true, 4'000'000'000 + cycle, frames, rate, outLeft.data(), outRight.data());
    CHECK(std::abs(outLeft[0] - 0.5f) < 1e-4f && std::abs(outRight[0] + 0.25f) < 1e-4f, "full mix %.3f %.3f", outLeft[0], outRight[0]);
}

void channelTestAlternates() {
    float leftPeak = 0, rightPeak = 0, leaked = 0;
    for (int i = 0; i < 48000; ++i) {
        const double seconds = i / 48000.0;
        const StereoSample sample = channelTestSample(seconds);
        const bool rightBeat = std::fmod(seconds, 1.0) >= 0.5;
        (rightBeat ? rightPeak : leftPeak) = std::max(rightBeat ? rightPeak : leftPeak, std::abs(rightBeat ? sample.right : sample.left));
        leaked = std::max(leaked, std::abs(rightBeat ? sample.left : sample.right));
    }
    CHECK(leftPeak > 0.3f && rightPeak > 0.3f, "peaks %.3f %.3f", leftPeak, rightPeak);
    CHECK(leaked == 0, "other channel must be silent, got %.3f", leaked);
    CHECK(speakerLayoutChannelCount(SpeakerLayout::stereoPair) == 2, "stereo pair has two channels");
    CHECK(speakerLayoutChannel(SpeakerLayout::stereoPair, 0) == SpeakerChannel::left
        && speakerLayoutChannel(SpeakerLayout::stereoPair, 1) == SpeakerChannel::right, "stereo pair order");
}

/// Feeds noise into a ring in 4096-frame chunks, paced like the file feeder, and checks every
/// rendered sample against the source while the ring wraps around.
void streamPlaysBitExact(int64_t joinAtMedia) {
    const double rate = 48000;
    const int sourceFrames = static_cast<int>(40 * rate);
    std::mt19937 random(1);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    std::vector<float> sourceLeft(static_cast<size_t>(sourceFrames)), sourceRight(static_cast<size_t>(sourceFrames));
    for (int i = 0; i < sourceFrames; ++i) {
        sourceLeft[static_cast<size_t>(i)] = noise(random);
        sourceRight[static_cast<size_t>(i)] = noise(random);
    }

    const PlaybackState playback{99, 5'000'000'000, 0};
    const int64_t startFrame = std::max<int64_t>(0, static_cast<int64_t>(static_cast<double>(joinAtMedia) * rate / 1e9));
    Ref<StreamBuffer> buffer = Ref<StreamBuffer>::adopt(StreamBuffer::create(rate, static_cast<int>(12 * rate), startFrame));
    int64_t fed = startFrame;
    auto feed = [&](int64_t heardMedia) {
        const int64_t target = static_cast<int64_t>(static_cast<double>(heardMedia) * rate / 1e9) + static_cast<int64_t>(8 * rate);
        while (buffer->endFrame() < std::max(target, startFrame + static_cast<int64_t>(8 * rate)) && fed < sourceFrames) {
            const int count = static_cast<int>(std::min<int64_t>(4096, sourceFrames - fed));
            buffer->append(&sourceLeft[static_cast<size_t>(fed)], &sourceRight[static_cast<size_t>(fed)], count);
            fed += count;
        }
    };

    Ref<SyncedRenderer> renderer = Ref<SyncedRenderer>::adopt(SyncedRenderer::create());
    renderer->setClockMap(ClockMap::identity());
    renderer->setPlayback(playback, 1, RenderSourceKind::stream, buffer.get());
    const int frames = 480;  // exactly 10 ms, so every buffer starts on an integer frame
    std::vector<float> left(frames), right(frames);
    int64_t host = joinAtMedia >= 0 ? playback.startLeaderTime + joinAtMedia : playback.startLeaderTime - 300'000'000;
    long compared = 0, mismatches = 0;
    feed(playback.mediaTimeAtLeaderTime(host));
    for (int k = 0; k < 2000; ++k) {
        if (k % 10 == 0) feed(playback.mediaTimeAtLeaderTime(host));
        renderer->render(true, host, frames, rate, left.data(), right.data());
        for (int i = 0; i < frames; ++i) {
            const int64_t media = playback.mediaTimeAtLeaderTime(host) + static_cast<int64_t>(i) * 1'000'000'000 / 48000;
            const long frame = std::lround(static_cast<double>(media) * rate / 1e9);
            if (frame < 0) continue;
            ++compared;
            if (left[static_cast<size_t>(i)] != sourceLeft[static_cast<size_t>(frame)] || right[static_cast<size_t>(i)] != sourceRight[static_cast<size_t>(frame)]) ++mismatches;
        }
        host += 10'000'000;
    }
    CHECK(mismatches == 0, "%ld of %ld frames differ", mismatches, compared);
    CHECK(buffer->validRange().lowerBound > startFrame, "ring did not wrap");
    CHECK(renderer->latestStats().jumps == 0 && renderer->latestStats().gaps == 0, "jumps %d gaps %d", renderer->latestStats().jumps, renderer->latestStats().gaps);
}

void streamGapsFade() {
    const double rate = 48000;
    Ref<StreamBuffer> buffer = Ref<StreamBuffer>::adopt(StreamBuffer::create(rate, 96000, 0));
    std::vector<float> half(10000, 0.5f);
    buffer->append(half.data(), half.data(), 10000);
    Ref<SyncedRenderer> renderer = Ref<SyncedRenderer>::adopt(SyncedRenderer::create());
    renderer->setClockMap(ClockMap::identity());
    renderer->setPlayback(PlaybackState{9, 0, 0}, 1, RenderSourceKind::stream, buffer.get());
    const int frames = 480;
    std::vector<float> left(frames), right(frames), out;
    int64_t host = 0;
    for (int k = 0; k < 60; ++k) {
        if (k == 35) buffer->append(half.data(), half.data(), 10000);
        renderer->render(true, host, frames, rate, left.data(), right.data());
        out.insert(out.end(), left.begin(), left.end());
        host += 10'000'000;
    }
    CHECK(out[9700] == 0.5f, "steady %.3f", out[9700]);
    CHECK(out[9900] > 0 && out[9900] < 0.5f, "fading out %.3f", out[9900]);
    CHECK(out[9997] < 0.01f && out[10005] == 0, "silent at the gap %.3f %.3f", out[9997], out[10005]);
    // Data resumes at callback 35 (frame 16800): it must fade back in, not jump to full level.
    CHECK(out[16800] < 0.01f && out[16810] > 0.0f && out[16810] < 0.1f && out[17200] == 0.5f, "fade in %.3f %.3f %.3f", out[16800], out[16810], out[17200]);
    CHECK(renderer->latestStats().gaps == 2, "gaps %d", renderer->latestStats().gaps);
}

void ringBufferConcurrency() {
    Ref<StreamBuffer> ring = Ref<StreamBuffer>::adopt(StreamBuffer::create(48000, 4096, 0));
    const int64_t total = 30'000'000;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        std::vector<float> chunk(256);
        for (int64_t f = 0; f < total; f += 256) {
            for (int i = 0; i < 256; ++i) chunk[static_cast<size_t>(i)] = static_cast<float>((f + i) % 1'000'000);
            ring->append(chunk.data(), chunk.data(), 256);
        }
        done = true;
    });
    long validated = 0, invalidated = 0, corrupt = 0;
    while (!done) {
        const FrameRange range = ring->validRange();
        if (range.count() <= 64) continue;
        // Read near the oldest frames to provoke laps.
        const int64_t from = range.lowerBound + range.count() / 4;
        bool bad = false;
        for (int64_t f = from; f < from + 32; ++f) {
            if (ring->sampleAt(f).left != static_cast<float>(f % 1'000'000)) bad = true;
        }
        if (ring->isStillValid(from)) {
            ++validated;
            if (bad) ++corrupt;
        } else {
            ++invalidated;
        }
    }
    writer.join();
    CHECK(corrupt == 0, "%ld corrupt reads passed validation (%ld validated, %ld invalidated)", corrupt, validated, invalidated);
    CHECK(validated > 1000, "only %ld validated reads", validated);
}

// MARK: - Protocol

/// Bytes produced by the original Swift encoders; the C++ encoders must match them exactly so
/// devices on either implementation interoperate.
void protocolMatchesGoldenBytes() {
    const PlaybackState state{0x1122334455667788ull, 1'234'567'890'123, -42};
    const std::pair<Bytes, const char*> cases[] = {
        {ClockSyncMessage::makePing(0x01020304, -5).encode(), "5341010104030201fbffffffffffffff"},
        {ClockSyncMessage::makePong(7, 0xDEADBEEF00000001ull, 1, 2, 3).encode(), "534101020700000001000000efbeadde010000000000000002000000000000000300000000000000"},
        {ClockSyncMessage::makePlayback(9, 0xABCD, state).encode(), "5341010309000000cdab0000000000008877665544332211cb04fb711f010000d6ffffffffffffff"},
        {ClockSyncMessage::makeAudioPort(0x55, 50829).encode(), "534101040000000055000000000000008dc6"},
        {ClockSyncMessage::makeHello(0x0102, Bytes{'a', 'b'}).encode(), "53410105000000000201000000000000026162"},
        {ClockSyncMessage::makeSpeakers(3, 0xAB, SpeakerLayout::stereoPair, SpeakerChannel::right, -1).encode(), "5341010603000000ab000000000000000102ffffffffffffffff"},
        {AudioStreamMessage::makeStreamStart(0x0A0B, 0x0C0D, 44100, 12345, AudioStreamCodec::alac, Bytes{1, 2, 3}).framed(), "27000000010b0a0000000000000d0c000000000000000000008088e5403930000000000000010300010203"},
        {AudioStreamMessage::makeAudio(1, -3, 2, Bytes{9, 8, 7, 6, 5, 4, 3, 2}).framed(), "1d000000020100000000000000fdffffffffffffff020000000908070605040302"},
        {AudioStreamMessage::makeBufferReport(0xFFFFFFFFFFFFFFFFull, int64_t{1} << 40).framed(), "1100000003ffffffffffffffff0000000000010000"},
    };
    for (const auto& [bytes, expected] : cases) {
        CHECK(hex(bytes) == expected, "\n    got      %s\n    expected %s", hex(bytes).c_str(), expected);
    }
}

void protocolRoundTrips() {
    const PlaybackState state{5, -7, 9};
    ClockSyncMessage message;
    Bytes bytes = ClockSyncMessage::makePlayback(3, 4, state).encode();
    CHECK(ClockSyncMessage::decode(bytes.data(), bytes.size(), message), "playback decode");
    CHECK(message.type == ClockSyncMessage::Type::playback && message.sequence == 3 && message.session == 4 && message.playback == state, "playback fields");
    bytes = ClockSyncMessage::makePong(1, 2, 3, 4, 5).encode();
    CHECK(ClockSyncMessage::decode(bytes.data(), bytes.size(), message) && message.t2 == 5, "pong");
    bytes.resize(bytes.size() - 1);
    CHECK(!ClockSyncMessage::decode(bytes.data(), bytes.size(), message), "truncated pong rejected");
    bytes = ClockSyncMessage::makeAudioPort(8, 65535).encode();
    CHECK(ClockSyncMessage::decode(bytes.data(), bytes.size(), message) && message.port == 65535, "audio port");
    bytes[0] = 0;
    CHECK(!ClockSyncMessage::decode(bytes.data(), bytes.size(), message), "bad magic rejected");

    bytes = ClockSyncMessage::makeHello(42, Bytes(300, 'x')).encode();
    CHECK(ClockSyncMessage::decode(bytes.data(), bytes.size(), message), "hello decode");
    CHECK(message.type == ClockSyncMessage::Type::hello && message.device == 42 && message.name.size() == 255, "hello fields, name cut to 255");
    bytes.pop_back();
    CHECK(!ClockSyncMessage::decode(bytes.data(), bytes.size(), message), "truncated hello rejected");
    bytes = ClockSyncMessage::makeSpeakers(7, 8, SpeakerLayout::stereoPair, SpeakerChannel::left, 123).encode();
    CHECK(ClockSyncMessage::decode(bytes.data(), bytes.size(), message), "speakers decode");
    CHECK(message.sequence == 7 && message.session == 8 && message.layout == SpeakerLayout::stereoPair && message.channel == SpeakerChannel::left
        && message.switchLeaderTime == 123, "speakers fields");
    bytes[16] = 9;  // a layout from the future
    CHECK(!ClockSyncMessage::decode(bytes.data(), bytes.size(), message), "unknown layout rejected");

    const Bytes framed = AudioStreamMessage::makeStreamStart(1, 2, 48000, -5, AudioStreamCodec::pcm16, Bytes{}).framed();
    const int64_t length = AudioStreamMessage::bodyLength(framed.data(), AudioStreamMessage::kHeaderLength);
    CHECK(length == static_cast<int64_t>(framed.size() - 4), "body length %lld", static_cast<long long>(length));
    AudioStreamMessage stream;
    CHECK(AudioStreamMessage::decodeBody(framed.data() + 4, framed.size() - 4, stream), "stream start decode");
    CHECK(stream.type == AudioStreamMessage::Type::streamStart && stream.sampleRate == 48000 && stream.frame == -5 && stream.cookie.empty(), "stream start fields");
    const uint8_t hugeHeader[4] = {0xff, 0xff, 0xff, 0x7f};
    CHECK(AudioStreamMessage::bodyLength(hugeHeader, 4) == -1, "oversized frame rejected");
}

void pcm16RoundTripsAllValues() {
    std::vector<float> left(65536), right(65536), backLeft(65536), backRight(65536);
    for (int i = 0; i < 65536; ++i) {
        left[static_cast<size_t>(i)] = static_cast<float>(i - 32768) / 32768.0f;
        right[static_cast<size_t>(65535 - i)] = left[static_cast<size_t>(i)];
    }
    Bytes bytes(65536 * 4);
    encodePCM16(left.data(), right.data(), 65536, bytes.data());
    CHECK(decodePCM16(bytes.data(), bytes.size(), 65536, backLeft.data(), backRight.data()), "decode");
    CHECK(backLeft == left && backRight == right, "16-bit values must round-trip exactly");
    CHECK(toPCM16(2.0f) == INT16_MAX && toPCM16(-2.0f) == INT16_MIN, "clamping");

    Ref<StreamBuffer> buffer = Ref<StreamBuffer>::adopt(StreamBuffer::create(48000, 65536, 100));
    buffer->append(left.data(), right.data(), 65536);
    std::vector<int16_t> interleaved(2 * 1000);
    CHECK(buffer->readInterleavedPCM16(100 + 500, 1000, interleaved.data()), "read from buffer");
    CHECK(interleaved[0] == static_cast<int16_t>(500 - 32768) && interleaved[1] == static_cast<int16_t>(65535 - 500 - 32768), "interleaved order");
    CHECK(!buffer->readInterleavedPCM16(50, 10, interleaved.data()), "frames before the buffer are refused");
}

// MARK: - ALAC

#include "fixtures/apple_alac_packets.inc"

Bytes fromHex(const char* digits) {
    Bytes bytes;
    auto value = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (; digits[0] != 0 && digits[1] != 0; digits += 2) {
        bytes.push_back(static_cast<uint8_t>(value(digits[0]) << 4 | value(digits[1])));
    }
    return bytes;
}

/// The signal `fixtures/generate_apple_alac.swift` encoded: a sawtooth with a little noise,
/// interleaved stereo.
std::vector<int16_t> alacTestSignal(int frames) {
    std::vector<int16_t> samples(static_cast<size_t>(frames) * 2);
    uint32_t state = 12345;
    for (int i = 0; i < frames; ++i) {
        state = state * 1664525u + 1013904223u;
        const int left = (i % 100) * 600 - 30000 + static_cast<int>((state >> 24) & 0x3F) - 32;
        const int right = left / 2 + static_cast<int>((state >> 8) & 0x0F) - 8;
        samples[static_cast<size_t>(2 * i)] = static_cast<int16_t>(left);
        samples[static_cast<size_t>(2 * i + 1)] = static_cast<int16_t>(right);
    }
    return samples;
}

/// Decodes `packet` and compares it with `expected` (interleaved, from `frame` on).
bool alacDecodesExactly(ALACDecoder& decoder, const Bytes& packet, int frames, const int16_t* expected) {
    std::vector<float> left(kALACFramesPerPacket), right(kALACFramesPerPacket);
    if (!decoder.decode(packet.data(), packet.size(), frames, left.data(), right.data())) {
        return false;
    }
    for (int i = 0; i < frames; ++i) {
        if (toPCM16(left[static_cast<size_t>(i)]) != expected[2 * i] || toPCM16(right[static_cast<size_t>(i)]) != expected[2 * i + 1]) {
            std::printf("  first difference at frame %d\n", i);
            return false;
        }
    }
    return true;
}

/// Apple hosts stream packets from AudioConverter; the reference decoder must reproduce them
/// bit for bit.
void alacDecodesApplePackets() {
    const Bytes cookie = fromHex(kAppleALACCookie);
    const Bytes packets[] = {fromHex(kAppleALACPacket0), fromHex(kAppleALACPacket1)};
    const std::vector<int16_t> signal = alacTestSignal(kAppleALACPacketFrames[0] + kAppleALACPacketFrames[1]);
    ALACDecoder decoder;
    CHECK(decoder.configure(cookie.data(), cookie.size()), "Apple cookie accepted");
    CHECK(alacDecodesExactly(decoder, packets[0], kAppleALACPacketFrames[0], signal.data()), "full packet");
    CHECK(alacDecodesExactly(decoder, packets[1], kAppleALACPacketFrames[1], signal.data() + 2 * kAppleALACPacketFrames[0]), "partial packet");

    std::vector<float> left(kALACFramesPerPacket), right(kALACFramesPerPacket);
    CHECK(!decoder.decode(packets[1].data(), packets[1].size(), kAppleALACPacketFrames[1] + 1, left.data(), right.data()), "frame count mismatch rejected");
}

void alacRoundTrips() {
    ALACEncoder encoder(kAppleALACSampleRate);
    CHECK(encoder.magicCookie().size() == 24, "cookie size %zu", encoder.magicCookie().size());
    // Same stream parameters as Apple's encoder, so either side can decode the other's packets.
    // Only maxFrameBytes (bytes 12...15), which Apple fills in and decoders ignore, differs.
    Bytes cookie = encoder.magicCookie();
    std::copy_n(fromHex(kAppleALACCookie).begin() + 12, 4, cookie.begin() + 12);
    CHECK(hex(cookie) == kAppleALACCookie, "cookie %s", hex(encoder.magicCookie()).c_str());
    ALACDecoder decoder;
    CHECK(decoder.configure(encoder.magicCookie().data(), encoder.magicCookie().size()), "own cookie accepted");

    std::vector<int16_t> extremes(2 * kALACFramesPerPacket);
    for (size_t i = 0; i < extremes.size(); ++i) {
        extremes[i] = (i / 3) % 2 == 0 ? INT16_MIN : INT16_MAX;
    }
    const std::vector<int16_t> silence(2 * kALACFramesPerPacket, 0);
    const std::vector<int16_t> signal = alacTestSignal(kALACFramesPerPacket);
    SplitMix64 random{7};
    std::vector<int16_t> noise(2 * kALACFramesPerPacket);
    for (auto& sample : noise) {
        sample = static_cast<int16_t>(random.next());
    }
    const std::pair<const std::vector<int16_t>*, int> cases[] = {
        {&signal, kALACFramesPerPacket}, {&signal, 1000}, {&silence, kALACFramesPerPacket}, {&extremes, kALACFramesPerPacket},
        {&noise, kALACFramesPerPacket}, {&noise, 1}, {&signal, kALACFramesPerPacket},
    };
    Bytes packet;
    for (const auto& [samples, frames] : cases) {
        CHECK(encoder.encode(samples->data(), frames, packet), "encode %d frames", frames);
        CHECK(alacDecodesExactly(decoder, packet, frames, samples->data()), "round trip of %d frames", frames);
    }
    CHECK(!encoder.encode(signal.data(), kALACFramesPerPacket + 1, packet), "oversized chunk rejected");
}

void alacRejectsGarbage() {
    ALACDecoder decoder;
    const Bytes cookie = fromHex(kAppleALACCookie);
    CHECK(!decoder.configure(cookie.data(), 8), "short cookie rejected");
    Bytes mono = cookie;
    mono[9] = 1;  // numChannels
    CHECK(!decoder.configure(mono.data(), mono.size()), "mono cookie rejected");
    std::vector<float> left(kALACFramesPerPacket), right(kALACFramesPerPacket);
    CHECK(!decoder.decode(cookie.data(), cookie.size(), 10, left.data(), right.data()), "unconfigured decoder refuses");

    CHECK(decoder.configure(cookie.data(), cookie.size()), "configure");
    const Bytes packet = fromHex(kAppleALACPacket1);
    // Truncated and corrupted packets must fail (or decode to something) without crashing.
    for (size_t length = 1; length < packet.size(); length += 7) {
        decoder.decode(packet.data(), length, kAppleALACPacketFrames[1], left.data(), right.data());
    }
    SplitMix64 random{99};
    for (int round = 0; round < 2000; ++round) {
        Bytes corrupted = packet;
        for (int flips = 0; flips < 4; ++flips) {
            corrupted[random.next() % corrupted.size()] ^= static_cast<uint8_t>(1u << (random.next() % 8));
        }
        decoder.decode(corrupted.data(), corrupted.size(), kAppleALACPacketFrames[1], left.data(), right.data());
    }
}

// MARK: - Networking

/// Polls `condition` for up to `seconds`.
bool waitUntil(const std::function<bool()>& condition, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return condition();
}

void serialQueueOrdersTasks() {
    SerialQueue queue("test");
    std::vector<int> order;
    std::mutex mutex;
    const auto push = [&](int value) {
        std::lock_guard<std::mutex> lock(mutex);
        order.push_back(value);
    };
    queue.asyncAfter(30'000'000, [&] { push(3); });
    queue.asyncAfter(10'000'000, [&] { push(2); });
    queue.async([&] { push(1); });
    queue.sync([&] { push(0); });
    CHECK(waitUntil([&] { std::lock_guard<std::mutex> lock(mutex); return order.size() == 4; }, 2), "all tasks ran");
    std::lock_guard<std::mutex> lock(mutex);
    CHECK((order == std::vector<int>{1, 0, 2, 3}), "order %d %d %d %d", order[0], order[1], order[2], order[3]);
}

/// A minimal leader on loopback: answers pings on a clock `offset` ahead of ours and answers
/// every hello with playback, the audio port and a speaker channel, like the Apple leader.
class FakeLeader {
public:
    explicit FakeLeader(int64_t offset, PlaybackState playback = PlaybackState{kPlaybackSourceMetronome, 123'456, 0}, uint16_t audioPort = 5555)
        : playback(playback), offset_(offset), audioPort_(audioPort) {
        socket_.open(net::Address::fromString("127.0.0.1", 0)->family());
        socket_.bind(0);
        port = socket_.localAddress().port();
        thread_ = std::thread([this] { run(); });
    }
    ~FakeLeader() {
        stop_ = true;
        thread_.join();
    }

    uint16_t port = 0;
    std::atomic<bool> answering{true};
    std::atomic<int> hellos{0};
    std::atomic<uint64_t> helloDevice{0};
    const PlaybackState playback;

private:
    void run() {
        uint8_t buffer[512];
        while (!stop_) {
            net::Address from;
            const int size = socket_.receive(buffer, sizeof(buffer), 20, &from);
            const int64_t t1 = hostClockNanoseconds() + offset_;
            ClockSyncMessage message;
            if (size <= 0 || !answering || !ClockSyncMessage::decode(buffer, static_cast<size_t>(size), message)) continue;
            if (message.type == ClockSyncMessage::Type::ping) {
                send(ClockSyncMessage::makePong(message.sequence, 42, message.t0, t1, hostClockNanoseconds() + offset_), from);
            } else if (message.type == ClockSyncMessage::Type::hello) {
                ++hellos;
                helloDevice = message.device;
                send(ClockSyncMessage::makePlayback(1, 42, playback), from);
                send(ClockSyncMessage::makeAudioPort(42, audioPort_), from);
                send(ClockSyncMessage::makeSpeakers(1, 42, SpeakerLayout::stereoPair, SpeakerChannel::right, 777), from);
            }
        }
    }
    void send(const ClockSyncMessage& message, const net::Address& to) {
        const Bytes bytes = message.encode();
        socket_.sendTo(bytes.data(), bytes.size(), to);
    }

    const int64_t offset_;
    const uint16_t audioPort_;
    net::UDPSocket socket_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

ClockSyncFollower::Configuration fastFollower() {
    ClockSyncFollower::Configuration configuration;
    configuration.probeSpacing = 2'000'000;
    configuration.replyGrace = 30'000'000;
    configuration.warmupRounds = 1000;
    configuration.warmupInterval = 20'000'000;
    configuration.silentRoundsBeforeReconnect = 3;
    configuration.reconnectDelay = 50'000'000;
    return configuration;
}

void followerSyncsWithLeader() {
    const int64_t offset = 5'000'000'000;
    FakeLeader leader(offset);
    std::atomic<int> resolves{0};
    const auto resolver = [&]() -> std::optional<net::Address> {
        ++resolves;
        return net::Address::fromString("127.0.0.1", leader.port);
    };
    ClockSyncFollower follower(resolver, DeviceIdentity{0xD00D, "Test"}, fastFollower());

    std::mutex mutex;
    std::optional<ClockMap> map;
    std::vector<PlaybackState> playbacks;
    std::vector<net::Address> endpoints;
    std::vector<std::pair<SpeakerChannel, int64_t>> channels;
    std::vector<ClockSyncFollower::State> states;
    follower.onRound = [&](const ClockSyncRound& round) {
        std::lock_guard<std::mutex> lock(mutex);
        map = round.map;
    };
    follower.onPlayback = [&](const PlaybackState& state) {
        std::lock_guard<std::mutex> lock(mutex);
        playbacks.push_back(state);
    };
    follower.onAudioEndpoint = [&](const net::Address& endpoint) {
        std::lock_guard<std::mutex> lock(mutex);
        endpoints.push_back(endpoint);
    };
    follower.onSpeakers = [&](SpeakerLayout, SpeakerChannel channel, int64_t time) {
        std::lock_guard<std::mutex> lock(mutex);
        channels.emplace_back(channel, time);
    };
    follower.onStateChange = [&](ClockSyncFollower::State state, const std::string&) {
        std::lock_guard<std::mutex> lock(mutex);
        states.push_back(state);
    };
    follower.start();

    CHECK(waitUntil([&] { return leader.hellos >= 5; }, 5), "hellos %d", leader.hellos.load());
    CHECK(waitUntil([&] { std::lock_guard<std::mutex> lock(mutex); return map.has_value(); }, 5), "a clock map");
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int64_t error = map ? map->offsetAtLocal(hostClockNanoseconds()) - offset : -1;
        CHECK(map && std::llabs(error) < 1'000'000, "offset error %lldns", static_cast<long long>(error));
        CHECK(leader.helloDevice == 0xD00D, "hello device");
        CHECK(playbacks.size() == 1 && playbacks[0] == leader.playback, "playback once, repeats ignored (%zu)", playbacks.size());
        CHECK(endpoints.size() == 1 && endpoints[0].toString() == "127.0.0.1:5555", "audio endpoint %s", endpoints.empty() ? "-" : endpoints[0].toString().c_str());
        CHECK(channels.size() == 1 && channels[0].first == SpeakerChannel::right && channels[0].second == 777, "speakers once");
        CHECK(!states.empty() && states.back() == ClockSyncFollower::State::syncing, "syncing");
    }

    // A leader that stops answering is looked up again.
    const int resolvesBefore = resolves;
    leader.answering = false;
    CHECK(waitUntil([&] { return resolves > resolvesBefore; }, 5), "reconnects after silence");
    leader.answering = true;
    CHECK(waitUntil([&] { std::lock_guard<std::mutex> lock(mutex); return !states.empty() && states.back() == ClockSyncFollower::State::syncing; }, 5), "syncing again");
    follower.stop();
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!states.empty() && states.back() == ClockSyncFollower::State::stopped, "stopped");
}

void audioStreamClientReceives() {
    net::TCPListener listener;
    CHECK(listener.listen(net::Address::fromString("127.0.0.1", 0)->family(), 0), "listen");
    const uint16_t port = listener.localAddress().port();
    const int frames[] = {4096, 4096, 100};
    const int total = 4096 + 4096 + 100;
    const std::vector<int16_t> signal = alacTestSignal(total);
    std::atomic<int64_t> reported{0};

    std::thread server([&] {
        net::TCPSocket connection = listener.accept(5000);
        if (!connection.isOpen()) return;
        const auto send = [&](const AudioStreamMessage& message) {
            const Bytes bytes = message.framed();
            connection.sendAll(bytes.data(), bytes.size());
        };
        ALACEncoder encoder(48000);
        send(AudioStreamMessage::makeStreamStart(9, 77, 48000, 1000, AudioStreamCodec::alac, encoder.magicCookie()));
        // A chunk of another stream, still in flight after a switch, is ignored.
        send(AudioStreamMessage::makeAudio(8, 0, 1, Bytes{0, 0, 0, 0}));
        int64_t frame = 1000;
        int offset = 0;
        for (int count : frames) {
            Bytes packet;
            encoder.encode(signal.data() + 2 * offset, count, packet);
            send(AudioStreamMessage::makeAudio(9, frame, count, packet));
            frame += count;
            offset += count;
        }
        // Then a PCM stream.
        send(AudioStreamMessage::makeStreamStart(10, 78, 44100, -50, AudioStreamCodec::pcm16, Bytes{}));
        Bytes pcm(4 * 10);
        for (int i = 0; i < 10; ++i) {
            pcm[static_cast<size_t>(4 * i)] = static_cast<uint8_t>(i);
        }
        send(AudioStreamMessage::makeAudio(10, -50, 10, pcm));
        // Read the client's buffer reports until it hangs up.
        std::atomic<bool> stop{false};
        uint8_t header[4];
        while (connection.readExactly(header, 4, stop) == net::TCPSocket::ReadResult::ok) {
            Bytes body(static_cast<size_t>(AudioStreamMessage::bodyLength(header, 4)));
            if (connection.readExactly(body.data(), body.size(), stop) != net::TCPSocket::ReadResult::ok) break;
            AudioStreamMessage message;
            if (AudioStreamMessage::decodeBody(body.data(), body.size(), message) && message.type == AudioStreamMessage::Type::bufferReport && message.streamID == 9) {
                reported = message.frame;
            }
        }
    });

    AudioStreamClient client(*net::Address::fromString("127.0.0.1", port));
    std::mutex mutex;
    std::vector<std::pair<uint64_t, Ref<StreamBuffer>>> streams;
    client.onStream = [&](uint64_t source, const Ref<StreamBuffer>& buffer) {
        std::lock_guard<std::mutex> lock(mutex);
        streams.emplace_back(source, buffer);
    };
    client.start();
    CHECK(waitUntil([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return streams.size() == 2 && streams[1].second->endFrame() == -40;
    }, 5), "both streams received");
    CHECK(waitUntil([&] { return reported == 1000 + total; }, 5), "buffer reported through %lld", static_cast<long long>(reported.load()));
    client.stop();
    server.join();

    std::lock_guard<std::mutex> lock(mutex);
    if (streams.size() != 2) return;
    const Ref<StreamBuffer>& first = streams[0].second;
    CHECK(streams[0].first == 77 && first->sampleRate() == 48000, "first stream");
    CHECK(first->validRange().lowerBound == 1000 && first->endFrame() == 1000 + total, "first stream range");
    bool exact = true;
    for (int i = 0; i < total && exact; ++i) {
        const StereoSample sample = first->sampleAt(1000 + i);
        exact = toPCM16(sample.left) == signal[static_cast<size_t>(2 * i)] && toPCM16(sample.right) == signal[static_cast<size_t>(2 * i + 1)];
    }
    CHECK(exact, "ALAC stream bit-exact");
    CHECK(streams[1].first == 78 && toPCM16(streams[1].second->sampleAt(-45).left) == 5, "PCM stream");
}

/// A follower session end to end: clock sync, the leader's playback, its speaker channel and its
/// audio stream.
void sessionFollowsLeader() {
    net::TCPListener listener;
    listener.listen(net::Address::fromString("127.0.0.1", 0)->family(), 0);
    const uint64_t file = 0xF11E;
    FakeLeader leader(1'000'000'000, PlaybackState{file, hostClockNanoseconds() + 1'000'000'000, 0}, listener.localAddress().port());
    std::thread server([&] {
        net::TCPSocket connection = listener.accept(5000);
        const Bytes start = AudioStreamMessage::makeStreamStart(1, file, 48000, 0, AudioStreamCodec::pcm16, Bytes{}).framed();
        const Bytes audio = AudioStreamMessage::makeAudio(1, 0, 4, Bytes(16, 0)).framed();
        connection.sendAll(start.data(), start.size());
        connection.sendAll(audio.data(), audio.size());
        // Hold the connection until the client hangs up.
        std::atomic<bool> stop{false};
        uint8_t byte;
        while (connection.readExactly(&byte, 1, stop) == net::TCPSocket::ReadResult::ok) {
        }
    });

    {
        GroupSession session(DeviceIdentity{7, "Session Test"});
        std::atomic<int> changes{0};
        session.onChange = [&] { ++changes; };
        session.setRole(GroupSession::Role::follower);
        session.follow("Fake", [&] { return net::Address::fromString("127.0.0.1", leader.port); });
        const bool ready = waitUntil([&] {
            const auto state = session.snapshot();
            return state.clockMap && state.isAudioConnected && state.playback == leader.playback && !state.isWaitingForAudio
                && state.channel == SpeakerChannel::right;
        }, 10);
        const auto state = session.snapshot();
        CHECK(ready, "map %d audio %d playback %d waiting %d channel %d", state.clockMap.has_value(), state.isAudioConnected,
            state.playback == leader.playback, state.isWaitingForAudio, static_cast<int>(state.channel));
        CHECK(state.connectedLeader == std::string("Fake") && state.followerState == ClockSyncFollower::State::syncing, "following");
        CHECK(state.remoteLayout == SpeakerLayout::stereoPair && !state.rounds.empty() && !state.log.empty() && changes > 0, "state published");

        session.setRole(GroupSession::Role::off);
        CHECK(waitUntil([&] {
            const auto state = session.snapshot();
            return state.role == GroupSession::Role::off && !state.connectedLeader && !state.playback.isPlaying() && state.channel == SpeakerChannel::full;
        }, 5), "left the group");
    }
    server.join();
}

void playbackPosition() {
    Ref<PlaybackPosition> position = Ref<PlaybackPosition>::adopt(PlaybackPosition::create());
    int64_t media = 0;
    CHECK(!position->heardMediaTime(0, media), "unknown without a clock map");
    position->setClockMap(ClockMap{0, 1000, 0});
    position->setPlayback(PlaybackState{3, 5000, 100});
    CHECK(position->heardMediaTime(10'000, media) && media == 100 + (11'000 - 5000), "media %lld", static_cast<long long>(media));
}

void sleepClock() {
    const int64_t first = hostClockNanoseconds();
    const int64_t slept = sleptNanoseconds();
    int64_t worst = 0;
    for (int i = 0; i < 200'000; ++i) worst = std::max(worst, std::llabs(sleptNanoseconds() - slept));
    CHECK(hostClockNanoseconds() >= first, "monotonic");
    CHECK(worst < kSleepDetectionThreshold, "sleep reading jitter %lldns", static_cast<long long>(worst));
}

}  // namespace

int main() {
    run("clock map inverse", clockMapInverse);
    run("estimator: clean link", estimatorCleanLink);
    run("estimator: noisy Wi-Fi", estimatorNoisyWiFi);
    run("estimator: very noisy", estimatorVeryNoisy);
    run("estimator: recovers from leader jump", estimatorRecoversFromLeaderJump);
    run("renderer: starts on time", rendererStartsOnTime);
    run("renderer: tracks DAC drift", rendererTracksDrift);
    run("renderer: routes channels", rendererRoutesChannels);
    run("renderer: channel test", channelTestAlternates);
    run("stream: bit-exact from start", [] { streamPlaysBitExact(-1); });
    run("stream: bit-exact late join", [] { streamPlaysBitExact(7'300'000'000); });
    run("stream: gaps fade", streamGapsFade);
    run("ring buffer: concurrent writer", ringBufferConcurrency);
    run("protocol: golden bytes", protocolMatchesGoldenBytes);
    run("protocol: round trips", protocolRoundTrips);
    run("pcm16: round trips", pcm16RoundTripsAllValues);
    run("alac: decodes Apple's packets", alacDecodesApplePackets);
    run("alac: round trips", alacRoundTrips);
    run("alac: rejects garbage", alacRejectsGarbage);
    run("serial queue", serialQueueOrdersTasks);
    run("follower: syncs with a leader", followerSyncsWithLeader);
    run("stream client: receives ALAC and PCM", audioStreamClientReceives);
    run("session: follows a leader", sessionFollowsLeader);
    run("playback position", playbackPosition);
    run("host clock", sleepClock);
    std::printf("%s\n", failures == 0 ? "All tests passed." : "Some tests FAILED.");
    return failures == 0 ? 0 : 1;
}
