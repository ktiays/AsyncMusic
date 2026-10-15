//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace asyncmusic {

namespace {

constexpr uint16_t kMagic = 0x4153;  // "AS"
constexpr uint8_t kVersion = 1;

/// Little-endian writer, independent of the host's byte order.
class Writer {
public:
    explicit Writer(Bytes& bytes) : bytes_(bytes) {}

    template <typename T>
    void write(T value) {
        uint64_t bits = 0;
        static_assert(sizeof(T) <= sizeof(bits), "integer too wide");
        std::memcpy(&bits, &value, sizeof(T));
        for (size_t i = 0; i < sizeof(T); ++i) bytes_.push_back(static_cast<uint8_t>(bits >> (8 * i)));
    }
    void write(const Bytes& data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }

private:
    Bytes& bytes_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    template <typename T>
    bool read(T& value) {
        if (position_ + sizeof(T) > size_) return false;
        uint64_t bits = 0;
        for (size_t i = 0; i < sizeof(T); ++i) bits |= static_cast<uint64_t>(data_[position_ + i]) << (8 * i);
        std::memcpy(&value, &bits, sizeof(T));
        position_ += sizeof(T);
        return true;
    }
    size_t remaining() const { return size_ - position_; }
    Bytes rest() {
        Bytes bytes(data_ + position_, data_ + size_);
        position_ = size_;
        return bytes;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t position_ = 0;
};

}  // namespace

// MARK: - ClockSyncMessage

ClockSyncMessage ClockSyncMessage::makePing(uint32_t sequence, int64_t t0) {
    ClockSyncMessage message;
    message.type = Type::ping;
    message.sequence = sequence;
    message.t0 = t0;
    return message;
}

ClockSyncMessage ClockSyncMessage::makePong(uint32_t sequence, uint64_t session, int64_t t0, int64_t t1, int64_t t2) {
    ClockSyncMessage message;
    message.type = Type::pong;
    message.sequence = sequence;
    message.session = session;
    message.t0 = t0;
    message.t1 = t1;
    message.t2 = t2;
    return message;
}

ClockSyncMessage ClockSyncMessage::makePlayback(uint32_t revision, uint64_t session, const PlaybackState& playback) {
    ClockSyncMessage message;
    message.type = Type::playback;
    message.sequence = revision;
    message.session = session;
    message.playback = playback;
    return message;
}

ClockSyncMessage ClockSyncMessage::makeAudioPort(uint64_t session, uint16_t port) {
    ClockSyncMessage message;
    message.type = Type::audioPort;
    message.session = session;
    message.port = port;
    return message;
}

Bytes ClockSyncMessage::encode() const {
    Bytes bytes;
    bytes.reserve(48);
    Writer writer(bytes);
    writer.write(kMagic);
    writer.write(kVersion);
    writer.write(static_cast<uint8_t>(type));
    writer.write(type == Type::audioPort ? uint32_t{0} : sequence);
    switch (type) {
    case Type::ping:
        writer.write(t0);
        break;
    case Type::pong:
        writer.write(session);
        writer.write(t0);
        writer.write(t1);
        writer.write(t2);
        break;
    case Type::playback:
        writer.write(session);
        writer.write(playback.source);
        writer.write(playback.startLeaderTime);
        writer.write(playback.startMediaTime);
        break;
    case Type::audioPort:
        writer.write(session);
        writer.write(port);
        break;
    }
    return bytes;
}

bool ClockSyncMessage::decode(const uint8_t* data, size_t size, ClockSyncMessage& message) {
    if (data == nullptr) return false;
    Reader reader(data, size);
    uint16_t magic = 0;
    uint8_t version = 0, type = 0;
    ClockSyncMessage result;
    if (!reader.read(magic) || magic != kMagic || !reader.read(version) || version != kVersion || !reader.read(type) || !reader.read(result.sequence)) {
        return false;
    }
    switch (type) {
    case static_cast<uint8_t>(Type::ping):
        result.type = Type::ping;
        if (!reader.read(result.t0)) return false;
        break;
    case static_cast<uint8_t>(Type::pong):
        result.type = Type::pong;
        if (!reader.read(result.session) || !reader.read(result.t0) || !reader.read(result.t1) || !reader.read(result.t2)) return false;
        break;
    case static_cast<uint8_t>(Type::playback):
        result.type = Type::playback;
        if (!reader.read(result.session) || !reader.read(result.playback.source) || !reader.read(result.playback.startLeaderTime) || !reader.read(result.playback.startMediaTime)) return false;
        break;
    case static_cast<uint8_t>(Type::audioPort):
        result.type = Type::audioPort;
        if (!reader.read(result.session) || !reader.read(result.port)) return false;
        break;
    default:
        return false;
    }
    message = result;
    return true;
}

// MARK: - AudioStreamMessage

AudioStreamMessage AudioStreamMessage::makeStreamStart(uint64_t streamID, uint64_t source, double sampleRate, int64_t startFrame, AudioStreamCodec codec, const Bytes& cookie) {
    AudioStreamMessage message;
    message.type = Type::streamStart;
    message.streamID = streamID;
    message.source = source;
    message.sampleRate = sampleRate;
    message.frame = startFrame;
    message.codec = codec;
    message.cookie = cookie;
    return message;
}

AudioStreamMessage AudioStreamMessage::makeAudio(uint64_t streamID, int64_t startFrame, int frameCount, const Bytes& payload) {
    AudioStreamMessage message;
    message.type = Type::audio;
    message.streamID = streamID;
    message.frame = startFrame;
    message.frameCount = frameCount;
    message.payload = payload;
    return message;
}

AudioStreamMessage AudioStreamMessage::makeBufferReport(uint64_t streamID, int64_t endFrame) {
    AudioStreamMessage message;
    message.type = Type::bufferReport;
    message.streamID = streamID;
    message.frame = endFrame;
    return message;
}

Bytes AudioStreamMessage::framed() const {
    Bytes body;
    Writer writer(body);
    writer.write(static_cast<uint8_t>(type));
    writer.write(streamID);
    switch (type) {
    case Type::streamStart:
        writer.write(source);
        writer.write(sampleRate);
        writer.write(frame);
        writer.write(static_cast<uint8_t>(codec));
        writer.write(static_cast<uint16_t>(cookie.size()));
        writer.write(cookie);
        break;
    case Type::audio:
        writer.write(frame);
        writer.write(static_cast<uint32_t>(frameCount));
        writer.write(payload);
        break;
    case Type::bufferReport:
        writer.write(frame);
        break;
    }
    Bytes bytes;
    bytes.reserve(kHeaderLength + body.size());
    Writer header(bytes);
    header.write(static_cast<uint32_t>(body.size()));
    header.write(body);
    return bytes;
}

int64_t AudioStreamMessage::bodyLength(const uint8_t* header, size_t size) {
    if (header == nullptr || size != kHeaderLength) return -1;
    uint32_t length = 0;
    Reader reader(header, size);
    reader.read(length);
    return length > 0 && length <= kMaximumBodyLength ? static_cast<int64_t>(length) : -1;
}

bool AudioStreamMessage::decodeBody(const uint8_t* body, size_t size, AudioStreamMessage& message) {
    if (body == nullptr) return false;
    Reader reader(body, size);
    uint8_t type = 0;
    AudioStreamMessage result;
    if (!reader.read(type) || !reader.read(result.streamID)) return false;
    switch (type) {
    case static_cast<uint8_t>(Type::streamStart): {
        result.type = Type::streamStart;
        uint8_t codec = 0;
        uint16_t cookieLength = 0;
        if (!reader.read(result.source) || !reader.read(result.sampleRate) || !reader.read(result.frame) || !reader.read(codec) || !reader.read(cookieLength)) return false;
        if (codec > static_cast<uint8_t>(AudioStreamCodec::alac) || reader.remaining() != cookieLength) return false;
        result.codec = static_cast<AudioStreamCodec>(codec);
        result.cookie = reader.rest();
        break;
    }
    case static_cast<uint8_t>(Type::audio): {
        result.type = Type::audio;
        uint32_t frameCount = 0;
        if (!reader.read(result.frame) || !reader.read(frameCount) || reader.remaining() == 0) return false;
        result.frameCount = static_cast<int>(frameCount);
        result.payload = reader.rest();
        break;
    }
    case static_cast<uint8_t>(Type::bufferReport):
        result.type = Type::bufferReport;
        if (!reader.read(result.frame)) return false;
        break;
    default:
        return false;
    }
    message = std::move(result);
    return true;
}

// MARK: - PCM16

int16_t toPCM16(float sample) {
    const long value = std::lround(static_cast<double>(sample) * 32768.0);
    return static_cast<int16_t>(std::clamp<long>(value, INT16_MIN, INT16_MAX));
}

void encodePCM16(const float* left, const float* right, int count, uint8_t* destination) {
    for (int i = 0; i < count; ++i) {
        const auto l = static_cast<uint16_t>(toPCM16(left[i]));
        const auto r = static_cast<uint16_t>(toPCM16(right[i]));
        destination[4 * i] = static_cast<uint8_t>(l);
        destination[4 * i + 1] = static_cast<uint8_t>(l >> 8);
        destination[4 * i + 2] = static_cast<uint8_t>(r);
        destination[4 * i + 3] = static_cast<uint8_t>(r >> 8);
    }
}

bool decodePCM16(const uint8_t* source, size_t size, int count, float* left, float* right) {
    if (count < 0 || size != static_cast<size_t>(count) * 4) return false;
    for (int i = 0; i < count; ++i) {
        const auto l = static_cast<int16_t>(source[4 * i] | (source[4 * i + 1] << 8));
        const auto r = static_cast<int16_t>(source[4 * i + 2] | (source[4 * i + 3] << 8));
        left[i] = static_cast<float>(l) / 32768.0f;
        right[i] = static_cast<float>(r) / 32768.0f;
    }
    return true;
}

}  // namespace asyncmusic
