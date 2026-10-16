//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include <memory>
#include <vector>

#include "asyncmusic/alac.hpp"
#include "asyncmusic/audio_stream.hpp"
#include "asyncmusic/protocol.hpp"
#include "text.hpp"

namespace asyncmusic {

AudioStreamClient::AudioStreamClient(net::Address endpoint) : endpoint_(std::move(endpoint)) {}

AudioStreamClient::~AudioStreamClient() {
    stop();
}

void AudioStreamClient::start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread([this] { run(); });
}

void AudioStreamClient::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    stopped_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
        if (onStateChange) onStateChange(State::stopped, "");
    }
}

bool AudioStreamClient::pause(int milliseconds) {
    std::unique_lock<std::mutex> lock(mutex_);
    return !stopped_.wait_for(lock, std::chrono::milliseconds(milliseconds), [this] { return stopping_.load(); });
}

void AudioStreamClient::log(const std::string& text) {
    if (onLog) onLog(text);
}

void AudioStreamClient::run() {
    if (onStateChange) onStateChange(State::connecting, "");
    while (!stopping_) {
        net::TCPSocket socket;
        if (!socket.connect(endpoint_, 3000, &stopping_)) {
            if (stopping_) break;
            if (onStateChange) onStateChange(State::reconnecting, "Could not connect: " + net::lastErrorDescription());
        } else {
            if (onStateChange) onStateChange(State::connected, "");
            receive(socket);
        }
        // The leader restarts the stream for every new connection anyway.
        if (!pause(1000)) break;
    }
}

void AudioStreamClient::receive(net::TCPSocket& socket) {
    uint64_t streamID = 0;
    bool hasStream = false;
    Ref<StreamBuffer> buffer;
    AudioStreamCodec codec = AudioStreamCodec::pcm16;
    std::unique_ptr<ALACDecoder> decoder;
    std::vector<float> left(kALACFramesPerPacket), right(kALACFramesPerPacket);
    std::vector<uint8_t> body;

    const auto fail = [this](const std::string& reason) {
        if (!stopping_ && onStateChange) onStateChange(State::reconnecting, reason);
    };

    while (true) {
        uint8_t header[AudioStreamMessage::kHeaderLength];
        auto result = socket.readExactly(header, sizeof(header), stopping_);
        if (result == net::TCPSocket::ReadResult::ok) {
            const int64_t length = AudioStreamMessage::bodyLength(header, sizeof(header));
            if (length <= 0) return fail("Malformed stream");
            body.resize(static_cast<size_t>(length));
            result = socket.readExactly(body.data(), body.size(), stopping_);
        }
        switch (result) {
        case net::TCPSocket::ReadResult::ok: break;
        case net::TCPSocket::ReadResult::stopped: return;
        case net::TCPSocket::ReadResult::closed: return fail("Connection closed");
        case net::TCPSocket::ReadResult::failed: return fail(net::lastErrorDescription());
        }

        AudioStreamMessage message;
        if (!AudioStreamMessage::decodeBody(body.data(), body.size(), message)) continue;
        switch (message.type) {
        case AudioStreamMessage::Type::streamStart: {
            if (!(message.sampleRate > 0)) break;
            if (message.codec == AudioStreamCodec::alac) {
                decoder = std::make_unique<ALACDecoder>();
                if (!decoder->configure(message.cookie.data(), message.cookie.size())) {
                    log(format("Cannot decode stream %016llx: unsupported ALAC cookie", static_cast<unsigned long long>(message.streamID)));
                    decoder.reset();
                    hasStream = false;
                    break;
                }
            } else {
                decoder.reset();
            }
            codec = message.codec;
            const int capacity = static_cast<int>(kBufferDuration * message.sampleRate);
            buffer = Ref<StreamBuffer>::adopt(StreamBuffer::create(message.sampleRate, capacity, message.frame));
            streamID = message.streamID;
            hasStream = true;
            log(format("Stream %016llx: %.0f Hz %s from %.2fs", static_cast<unsigned long long>(streamID), message.sampleRate,
                codec == AudioStreamCodec::alac ? "ALAC" : "PCM", static_cast<double>(message.frame) / message.sampleRate));
            if (onStream) onStream(message.source, buffer);
            break;
        }
        case AudioStreamMessage::Type::bufferReport:
            break;
        case AudioStreamMessage::Type::audio: {
            // Chunks still in flight from a previous stream are expected after a switch.
            if (!hasStream || message.streamID != streamID) break;
            if (message.frame != buffer->endFrame()) {
                log(format("Stream discontinuity at frame %lld, expected %lld", static_cast<long long>(message.frame), static_cast<long long>(buffer->endFrame())));
                break;
            }
            const int count = message.frameCount;
            if (count <= 0 || (codec == AudioStreamCodec::alac && count > kALACFramesPerPacket)) break;
            if (static_cast<size_t>(count) > left.size()) {
                // PCM chunks are bounded only by the frame size limit.
                left.resize(static_cast<size_t>(count));
                right.resize(static_cast<size_t>(count));
            }
            bool decoded = false;
            if (codec == AudioStreamCodec::alac) {
                decoded = decoder && decoder->decode(message.payload.data(), message.payload.size(), count, left.data(), right.data());
            } else {
                decoded = decodePCM16(message.payload.data(), message.payload.size(), count, left.data(), right.data());
            }
            if (!decoded) {
                log(format("Could not decode chunk at frame %lld", static_cast<long long>(message.frame)));
                break;
            }
            buffer->append(left.data(), right.data(), count);
            const Bytes report = AudioStreamMessage::makeBufferReport(streamID, buffer->endFrame()).framed();
            if (!socket.sendAll(report.data(), report.size())) return fail(net::lastErrorDescription());
            break;
        }
        }
    }
}

}  // namespace asyncmusic
