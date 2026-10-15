//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

// Self-contained tests for the core; no framework needed so they build anywhere CMake does.

#include <asyncmusic/asyncmusic.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
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
    run("stream: bit-exact from start", [] { streamPlaysBitExact(-1); });
    run("stream: bit-exact late join", [] { streamPlaysBitExact(7'300'000'000); });
    run("stream: gaps fade", streamGapsFade);
    run("ring buffer: concurrent writer", ringBufferConcurrency);
    run("protocol: golden bytes", protocolMatchesGoldenBytes);
    run("protocol: round trips", protocolRoundTrips);
    run("pcm16: round trips", pcm16RoundTripsAllValues);
    run("playback position", playbackPosition);
    run("host clock", sleepClock);
    std::printf("%s\n", failures == 0 ? "All tests passed." : "Some tests FAILED.");
    return failures == 0 ? 0 : 1;
}
