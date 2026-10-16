//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/alac.hpp"

#include <cstring>

#include "ALACAudioTypes.h"
#include "ALACBitUtilities.h"
#include "ALACDecoder.h"
#include "ALACEncoder.h"

namespace asyncmusic {

namespace {

constexpr uint32_t kChannels = 2;
constexpr uint32_t kBytesPerFrame = kChannels * sizeof(int16_t);
/// The encoder's worst case: 5 bytes per sample (escaped 32-bit) plus headers.
constexpr size_t kMaximumPacketBytes = kALACFramesPerPacket * kChannels * 5 + kALACMaxEscapeHeaderBytes + 64;
/// Room after a packet for the reference decoder, which does not check where its bit reader
/// is: a corrupt packet can make it read up to a few bits per sample per channel past the end,
/// plus the skipped shift bits. Packets are copied into a buffer this much longer.
constexpr size_t kOverreadMargin = 128 * 1024;

AudioFormatDescription pcmFormat(double sampleRate) {
    AudioFormatDescription format{};
    format.mSampleRate = sampleRate;
    format.mFormatID = kALACFormatLinearPCM;
    format.mFormatFlags = kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked | kALACFormatFlagsNativeEndian;
    format.mBytesPerPacket = kBytesPerFrame;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = kBytesPerFrame;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 16;
    return format;
}

AudioFormatDescription alacFormat(double sampleRate) {
    AudioFormatDescription format{};
    format.mSampleRate = sampleRate;
    format.mFormatID = kALACFormatAppleLossless;
    // The 16-bit source flag, as in kAppleLosslessFormatFlag_16BitSourceData.
    format.mFormatFlags = 1;
    format.mFramesPerPacket = kALACFramesPerPacket;
    format.mChannelsPerFrame = kChannels;
    return format;
}

}  // namespace

// MARK: - Encoder

struct ALACEncoder::Implementation {
    ::ALACEncoder encoder;
    AudioFormatDescription input;
    AudioFormatDescription output;
};

ALACEncoder::ALACEncoder(double sampleRate) : implementation_(new Implementation), sampleRate_(sampleRate) {
    auto& codec = *implementation_;
    codec.input = pcmFormat(sampleRate);
    codec.output = alacFormat(sampleRate);
    codec.encoder.SetFrameSize(kALACFramesPerPacket);
    codec.encoder.InitializeEncoder(codec.output);

    uint32_t size = codec.encoder.GetMagicCookieSize(kChannels);
    cookie_.resize(size);
    codec.encoder.GetMagicCookie(cookie_.data(), &size);
    cookie_.resize(size);
}

ALACEncoder::~ALACEncoder() = default;

bool ALACEncoder::encode(const int16_t* interleaved, int count, std::vector<uint8_t>& packet) {
    if (interleaved == nullptr || count <= 0 || count > kALACFramesPerPacket) {
        return false;
    }
    auto& codec = *implementation_;
    packet.resize(kMaximumPacketBytes);
    auto bytes = static_cast<int32_t>(static_cast<uint32_t>(count) * kBytesPerFrame);
    // The encoder only reads its input.
    auto* input = reinterpret_cast<unsigned char*>(const_cast<int16_t*>(interleaved));
    if (codec.encoder.Encode(codec.input, codec.output, input, packet.data(), &bytes) != ALAC_noErr || bytes <= 0) {
        packet.clear();
        return false;
    }
    packet.resize(static_cast<size_t>(bytes));
    return true;
}

// MARK: - Decoder

struct ALACDecoder::Implementation {
    ::ALACDecoder decoder;
    std::vector<uint8_t> packet = std::vector<uint8_t>(kMaximumPacketBytes + kOverreadMargin);
    std::vector<int16_t> samples;
};

ALACDecoder::ALACDecoder() = default;
ALACDecoder::~ALACDecoder() = default;

bool ALACDecoder::configure(const uint8_t* cookie, size_t size) {
    implementation_.reset();
    // The reference decoder reads up to 24 bytes before checking the size.
    if (cookie == nullptr || size < sizeof(ALACSpecificConfig) || size > 1024) {
        return false;
    }
    auto codec = std::make_unique<Implementation>();
    std::vector<uint8_t> copy(cookie, cookie + size);
    if (codec->decoder.Init(copy.data(), static_cast<uint32_t>(copy.size())) != ALAC_noErr) {
        return false;
    }
    const auto& config = codec->decoder.mConfig;
    if (config.bitDepth != 16 || config.numChannels != kChannels || config.frameLength == 0
        || config.frameLength > static_cast<uint32_t>(kALACFramesPerPacket)) {
        return false;
    }
    codec->samples.resize(static_cast<size_t>(config.frameLength) * kChannels);
    implementation_ = std::move(codec);
    return true;
}

bool ALACDecoder::decode(const uint8_t* packet, size_t size, int frameCount, float* left, float* right) {
    if (!implementation_ || packet == nullptr || size == 0 || size > kMaximumPacketBytes || frameCount <= 0) {
        return false;
    }
    auto& codec = *implementation_;
    const uint32_t frameLength = codec.decoder.mConfig.frameLength;
    if (static_cast<uint32_t>(frameCount) > frameLength) {
        return false;
    }
    std::memcpy(codec.packet.data(), packet, size);
    std::memset(codec.packet.data() + size, 0, codec.packet.size() - size);

    BitBuffer bits;
    BitBufferInit(&bits, codec.packet.data(), static_cast<uint32_t>(size));
    uint32_t decoded = 0;
    const int32_t status = codec.decoder.Decode(&bits, reinterpret_cast<uint8_t*>(codec.samples.data()), frameLength, kChannels, &decoded);
    if (status != ALAC_noErr || decoded != static_cast<uint32_t>(frameCount)) {
        return false;
    }
    for (int i = 0; i < frameCount; ++i) {
        left[i] = static_cast<float>(codec.samples[static_cast<size_t>(2 * i)]) / 32768.0f;
        right[i] = static_cast<float>(codec.samples[static_cast<size_t>(2 * i + 1)]) / 32768.0f;
    }
    return true;
}

}  // namespace asyncmusic
