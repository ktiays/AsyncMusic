//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/media.hpp"

#include <algorithm>
#include <cmath>

#include "asyncmusic/host_clock.hpp"

namespace asyncmusic {

uint64_t mediaFileID(const uint8_t digest[32]) {
    uint64_t hash = 0;
    for (int i = 7; i >= 0; --i) hash = hash << 8 | digest[i];
    return std::max(hash, kPlaybackSourceFirstFile);
}

FileStreamFeeder::FileStreamFeeder(std::shared_ptr<const MediaFile> file, Ref<StreamBuffer> buffer, int64_t startFrame, Ref<PlaybackPosition> position,
    Configuration configuration)
    : file_(std::move(file)), buffer_(std::move(buffer)), startFrame_(startFrame), position_(std::move(position)), configuration_(configuration),
      queue_("asyncmusic.file-stream") {}

FileStreamFeeder::~FileStreamFeeder() {
    stop();
    queue_.shutdown();
}

void FileStreamFeeder::start() {
    queue_.async([this] {
        decoder_ = file_->openDecoder ? file_->openDecoder() : nullptr;
        if (!decoder_ || !decoder_->seek(std::clamp<int64_t>(startFrame_, 0, file_->frameCount))) {
            finish();
            return;
        }
        left_.resize(static_cast<size_t>(configuration_.chunkFrames));
        right_.resize(static_cast<size_t>(configuration_.chunkFrames));
        fill();
    });
}

void FileStreamFeeder::stop() {
    queue_.sync([this] { finish(); });
}

void FileStreamFeeder::finish() {
    finished_ = true;
    decoder_.reset();
}

/// Decodes until the buffer reaches `aheadDuration` past the playing position.
void FileStreamFeeder::fill() {
    if (finished_ || !decoder_) return;
    const double rate = buffer_->sampleRate();
    int64_t heard = 0;
    const bool hasPosition = position_->heardMediaTime(hostClockNanoseconds(), heard);
    const int64_t playingFrame = static_cast<int64_t>(std::floor(static_cast<double>(heard) * rate / 1e9));
    // Until the position is known, just prebuffer from the start.
    const int64_t target = (hasPosition ? playingFrame : startFrame_) + static_cast<int64_t>(configuration_.aheadDuration * rate);

    if (hasPosition && playingFrame - buffer_->endFrame() > static_cast<int64_t>(configuration_.stallThreshold * rate) && buffer_->endFrame() < file_->frameCount
        && !stalled_) {
        stalled_ = true;
        if (onStall) onStall();
        return;
    }
    while (buffer_->endFrame() < target) {
        const int count = decoder_->read(left_.data(), right_.data(), configuration_.chunkFrames);
        if (count <= 0) {
            finish();
            return;
        }
        buffer_->append(left_.data(), right_.data(), count);
    }
    queue_.asyncAfter(configuration_.interval, [this] { fill(); });
}

}  // namespace asyncmusic
