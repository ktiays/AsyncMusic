//
//  Created by ktiays on 2026/10/22.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/media.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace asyncmusic::android {

/// Opens a local audio file with the NDK's media extractor and codecs (MP3, AAC, FLAC, Opus,
/// Vorbis, WAV, ...): reads its format and length, and returns a `MediaFile` whose decoders
/// produce stereo float at the file's own rate, seeking sample-accurately. `digest` is the
/// SHA-256 of the file's contents, which gives it the same ID as on every other device. Decodes
/// the start of the file once; call it off the UI thread. Returns null and sets `error` on
/// failure.
std::shared_ptr<MediaFile> loadMediaFile(const std::string& path, const std::string& name, const uint8_t digest[32], std::string& error);

}  // namespace asyncmusic::android
