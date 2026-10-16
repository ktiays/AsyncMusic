//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <asyncmusic/media.hpp>

#include <memory>
#include <string>

namespace asyncmusic::windows {

/// Opens a local audio file with Media Foundation (MP3, AAC/ALAC in M4A, WAV, FLAC, WMA, ...):
/// hashes it for its ID, reads its format and length, and returns a `MediaFile` whose decoders
/// produce stereo float at the file's own rate, seeking sample-accurately. Reads the whole file
/// once; call it off the UI thread. Returns null and sets `error` on failure.
std::shared_ptr<MediaFile> loadMediaFile(const std::wstring& path, std::string& error);

/// Media Foundation must be started once per process before `loadMediaFile`.
class MediaFoundationScope {
public:
    MediaFoundationScope();
    ~MediaFoundationScope();
    MediaFoundationScope(const MediaFoundationScope&) = delete;
    MediaFoundationScope& operator=(const MediaFoundationScope&) = delete;

private:
    bool started_ = false;
};

}  // namespace asyncmusic::windows
