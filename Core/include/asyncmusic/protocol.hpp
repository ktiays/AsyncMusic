//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "config.hpp"
#include "playback.hpp"
#include "speakers.hpp"

AM_ASSUME_NONNULL_BEGIN

namespace asyncmusic {

using Bytes = std::vector<uint8_t>;

/// Copies `size` bytes into a `Bytes` (convenient for callers holding a raw buffer).
inline Bytes makeBytes(const uint8_t* AM_NULLABLE data, size_t size) {
    return data == nullptr ? Bytes() : Bytes(data, data + size);
}

// MARK: - Clock sync (UDP)

/// Longest name a `hello` carries, in bytes; longer ones are cut (callers should cut at a
/// character boundary first).
constexpr size_t kClockSyncMaximumNameLength = 255;

/// Datagrams of the clock sync channel. Fixed-size, little-endian:
///
///     header:    magic(2) = "AS" version(1) = 1 type(1) sequence(4)
///     ping:      header t0(8)
///     pong:      header session(8) t0(8) t1(8) t2(8)
///     playback:  header(sequence = revision) session(8) source(8) startLeaderTime(8) startMediaTime(8)
///     audioPort: header(sequence unused) session(8) port(2)
///     hello:     header(sequence unused) device(8) nameLength(1) name(nameLength, UTF-8)
///     speakers:  header(sequence = revision) session(8) layout(1) channel(1) switchLeaderTime(8)
///
/// `session` identifies one run of the leader's timeline; a new value means followers must
/// discard their clock estimate. Playback is repeated periodically since UDP may drop it, and
/// `revision` increases with every change within a session. `audioPort` is the TCP port of
/// the leader's audio stream on the same host as the clock service.
///
/// Followers introduce themselves with `hello` (a stable device ID and a display name),
/// repeated every round. The leader answers each with `speakers`: the group's layout and the
/// channel that follower plays from leader time `switchLeaderTime` on, so all devices switch
/// together. Like playback it is repeated, with its own revision. Devices that predate a
/// message type, or a layout or channel value, reject the datagram and keep playing the full mix.
struct ClockSyncMessage {
    enum class AM_CLOSED_ENUM Type : uint8_t {
        ping = 1,
        pong = 2,
        playback = 3,
        audioPort = 4,
        hello = 5,
        speakers = 6,
    };

    Type type = Type::ping;
    /// Ping/pong sequence, or the playback revision.
    uint32_t sequence = 0;
    uint64_t session = 0;
    int64_t t0 = 0;
    int64_t t1 = 0;
    int64_t t2 = 0;
    PlaybackState playback;
    uint16_t port = 0;
    /// `hello`: the follower's stable ID and its display name (UTF-8).
    uint64_t device = 0;
    Bytes name;
    /// `speakers`
    SpeakerLayout layout = SpeakerLayout::everywhere;
    SpeakerChannel channel = SpeakerChannel::full;
    int64_t switchLeaderTime = 0;

    static ClockSyncMessage makePing(uint32_t sequence, int64_t t0);
    static ClockSyncMessage makePong(uint32_t sequence, uint64_t session, int64_t t0, int64_t t1, int64_t t2);
    static ClockSyncMessage makePlayback(uint32_t revision, uint64_t session, const PlaybackState& playback);
    static ClockSyncMessage makeAudioPort(uint64_t session, uint16_t port);
    static ClockSyncMessage makeHello(uint64_t device, const Bytes& name);
    static ClockSyncMessage makeSpeakers(uint32_t revision, uint64_t session, SpeakerLayout layout, SpeakerChannel channel, int64_t switchLeaderTime);

    Bytes encode() const;
    /// Returns false for anything that is not a well-formed message of a known type.
    static bool decode(const uint8_t* AM_NULLABLE data, size_t size, ClockSyncMessage& message);
};

// MARK: - Audio stream (TCP)

enum class AM_CLOSED_ENUM AudioStreamCodec : uint8_t {
    /// Interleaved little-endian Int16 stereo.
    pcm16 = 0,
    /// One Apple Lossless packet per chunk; the magic cookie comes with the stream start.
    alac = 1,
};

/// Length of an audio stream frame header (the body length).
constexpr size_t kAudioStreamHeaderLength = 4;

/// Messages on the audio stream connection. Each is sent as a frame:
///
///     length(4, of what follows) type(1) payload
///
///     streamStart:  streamID(8) source(8) sampleRate(8, IEEE double) startFrame(8) codec(1) cookieLength(2) cookie
///     audio:        streamID(8) startFrame(8) frameCount(4) payload (codec-specific)
///     bufferReport: streamID(8) endFrame(8)      (follower -> leader)
///
/// Frame numbers are in the source's own sample rate, the same numbering the renderer uses.
/// Audio for a stream follows contiguously from its `startFrame`; a follower reports how far
/// its buffer reaches after each chunk, which the leader uses for the adaptive start.
struct AudioStreamMessage {
    enum class AM_CLOSED_ENUM Type : uint8_t {
        streamStart = 1,
        audio = 2,
        bufferReport = 3,
    };

    static constexpr size_t kHeaderLength = kAudioStreamHeaderLength;
    /// Upper bound on a frame's body, to reject garbage before allocating for it.
    static constexpr size_t kMaximumBodyLength = 1u << 20;

    Type type = Type::audio;
    uint64_t streamID = 0;
    uint64_t source = 0;
    double sampleRate = 0;
    /// `streamStart` and `audio`: first frame. `bufferReport`: end of the buffered range.
    int64_t frame = 0;
    AudioStreamCodec codec = AudioStreamCodec::pcm16;
    Bytes cookie;
    int frameCount = 0;
    Bytes payload;

    static AudioStreamMessage makeStreamStart(uint64_t streamID, uint64_t source, double sampleRate, int64_t startFrame, AudioStreamCodec codec, const Bytes& cookie);
    static AudioStreamMessage makeAudio(uint64_t streamID, int64_t startFrame, int frameCount, const Bytes& payload);
    static AudioStreamMessage makeBufferReport(uint64_t streamID, int64_t endFrame);

    /// The complete frame, header included.
    Bytes framed() const;
    /// The body length announced by a 4-byte header, or -1 if it is out of bounds.
    static int64_t bodyLength(const uint8_t* AM_NULLABLE header, size_t size);
    /// Decodes a frame body (everything after the header).
    static bool decodeBody(const uint8_t* AM_NULLABLE body, size_t size, AudioStreamMessage& message);
};

// MARK: - PCM16

/// Float sample to 16-bit, scaled by 2^15 so 16-bit sources round-trip exactly.
int16_t toPCM16(float sample);
/// Interleaves two float channels into little-endian Int16 stereo bytes (`4 * count` bytes).
void encodePCM16(const float* left, const float* right, int count, uint8_t* destination);
/// Splits little-endian Int16 stereo bytes into float channels; `size` must be `4 * count`.
bool decodePCM16(const uint8_t* source, size_t size, int count, float* left, float* right);

}  // namespace asyncmusic

AM_ASSUME_NONNULL_END
