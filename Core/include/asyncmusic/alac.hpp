//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace asyncmusic {

/// Frames per ALAC packet, and so per audio stream chunk.
constexpr int kALACFramesPerPacket = 4096;

/// Apple Lossless at 16 bits, stereo, one packet per stream chunk (Apple's open-source
/// reference codec, in `third_party/alac`).
///
/// Packets decode independently and carry no encoder delay, so a chunk of N frames at frame F
/// decodes to exactly those N frames at F. The decoder needs the encoder's magic cookie, which
/// the stream start carries. Interoperates with Apple's AudioConverter in both directions.
class ALACEncoder {
public:
    explicit ALACEncoder(double sampleRate);
    ~ALACEncoder();
    ALACEncoder(const ALACEncoder&) = delete;
    ALACEncoder& operator=(const ALACEncoder&) = delete;

    double sampleRate() const { return sampleRate_; }
    /// The `ALACSpecificConfig` the decoder needs (24 bytes).
    const std::vector<uint8_t>& magicCookie() const { return cookie_; }

    /// Encodes `count` frames (1...kALACFramesPerPacket) of interleaved 16-bit stereo into
    /// one packet, replacing the contents of `packet`.
    bool encode(const int16_t* interleaved, int count, std::vector<uint8_t>& packet);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
    double sampleRate_;
    std::vector<uint8_t> cookie_;
};

class ALACDecoder {
public:
    ALACDecoder();
    ~ALACDecoder();
    ALACDecoder(const ALACDecoder&) = delete;
    ALACDecoder& operator=(const ALACDecoder&) = delete;

    /// Accepts a 16-bit stereo stream's cookie (plain, or inside 'frma'/'alac' atoms).
    bool configure(const uint8_t* cookie, size_t size);

    /// Decodes one packet that must hold exactly `frameCount` frames into non-interleaved
    /// floats (scaled by 2^-15, like `decodePCM16`).
    bool decode(const uint8_t* packet, size_t size, int frameCount, float* left, float* right);

private:
    struct Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace asyncmusic
