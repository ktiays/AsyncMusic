//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "playback.hpp"
#include "ref_counted.hpp"
#include "serial_queue.hpp"
#include "stream_buffer.hpp"

namespace asyncmusic {

/// Decodes one audio file to stereo float at the file's own sample rate. Implemented by the
/// platform (Media Foundation, AVFoundation, ...). Used from one thread at a time.
class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    /// Positions the decoder so that the next `read` starts exactly at `frame`.
    virtual bool seek(int64_t frame) = 0;
    /// Decodes up to `capacity` frames; returns how many, 0 at the end of the file, -1 on error.
    /// Mono files come out on both channels.
    virtual int read(float* left, float* right, int capacity) = 0;
};

/// A local audio file that can be played. Only its header is read up front; the audio is
/// decoded a few seconds ahead of playback by `FileStreamFeeder`.
struct MediaFile {
    /// Derived from the file's bytes (see `mediaFileID`), so the same file has the same ID on
    /// every device.
    uint64_t id = 0;
    std::string name;
    double sampleRate = 0;
    int64_t frameCount = 0;
    /// Opens a new decoder for the file, or returns null if it cannot be read any more.
    std::function<std::unique_ptr<AudioDecoder>()> openDecoder;

    double duration() const { return sampleRate > 0 ? static_cast<double>(frameCount) / sampleRate : 0; }
};

/// The ID of a file whose contents have the SHA-256 `digest`: its first eight bytes as a
/// little-endian integer, kept clear of the IDs reserved for generated sources. The Apple app
/// derives it the same way.
uint64_t mediaFileID(const uint8_t digest[32]);

/// Decodes a `MediaFile` into a `StreamBuffer` in small chunks, staying `aheadDuration` ahead of
/// playback. A port of the Apple app's `FileStreamFeeder`: the local stand-in for a network
/// stream, paced the same way, from the media time being heard.
class FileStreamFeeder {
public:
    struct Configuration {
        /// How far ahead of the playing position to decode. Must stay well below the buffer's
        /// capacity, since frames older than the capacity before the newest are overwritten.
        double aheadDuration = 8.0;
        int chunkFrames = 4096;
        int64_t interval = 100'000'000;
        /// Falling this far behind the playing position counts as a stall.
        double stallThreshold = 0.5;
    };

    /// `buffer` must have been created at `startFrame`.
    FileStreamFeeder(std::shared_ptr<const MediaFile> file, Ref<StreamBuffer> buffer, int64_t startFrame, Ref<PlaybackPosition> position,
        Configuration configuration);
    ~FileStreamFeeder();
    FileStreamFeeder(const FileStreamFeeder&) = delete;
    FileStreamFeeder& operator=(const FileStreamFeeder&) = delete;

    /// Called on the feeder's thread when decoding fell behind playback. The buffer is
    /// append-only, so the owner should start a new stream. Set before `start()`.
    std::function<void()> onStall;

    const Ref<StreamBuffer>& buffer() const { return buffer_; }

    void start();
    /// Stops decoding and waits for the feeder's thread.
    void stop();

private:
    void fill();
    void finish();

    const std::shared_ptr<const MediaFile> file_;
    const Ref<StreamBuffer> buffer_;
    const int64_t startFrame_;
    const Ref<PlaybackPosition> position_;
    const Configuration configuration_;
    SerialQueue queue_;
    // Confined to `queue_`.
    std::unique_ptr<AudioDecoder> decoder_;
    std::vector<float> left_, right_;
    bool finished_ = false;
    bool stalled_ = false;
};

}  // namespace asyncmusic
