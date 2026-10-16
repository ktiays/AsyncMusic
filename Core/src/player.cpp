//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/player.hpp"

#include <algorithm>
#include <cmath>
#include <random>

#include "asyncmusic/host_clock.hpp"

namespace asyncmusic {

namespace {
/// How long a swapped-out buffer is kept for the render thread.
constexpr auto kRetireDelay = std::chrono::seconds(2);

uint64_t randomStreamID() {
    std::random_device device;
    return (static_cast<uint64_t>(device()) << 32) ^ device();
}
}  // namespace

Player::Player(std::function<void(std::function<void()>)> post)
    : post_(std::move(post)), renderer_(Ref<SyncedRenderer>::adopt(SyncedRenderer::create())),
      position_(Ref<PlaybackPosition>::adopt(PlaybackPosition::create())) {}

Player::~Player() {
    if (activeFeeder_) activeFeeder_->stop();
}

void Player::setClockMap(const std::optional<ClockMap>& map) {
    const bool wasUnknown = !clockMap_;
    clockMap_ = map;
    if (map) {
        renderer_->setClockMap(*map);
        position_->setClockMap(*map);
    } else {
        renderer_->clearClockMap();
        position_->clearClockMap();
    }
    // A file stream can only start once it is known where playback is.
    if (wasUnknown && map && !activeFeeder_ && files_.count(playback_.source) != 0) {
        applySource(true);
    }
}

void Player::setPlayback(const PlaybackState& playback) {
    // Re-applying the same state would needlessly reposition the read head.
    if (playback == playback_) return;
    const PlaybackState previous = playback_;
    playback_ = playback;
    position_->setPlayback(playback);
    if (activeFeeder_ && canKeepStream(previous)) {
        // Only the schedule changed (e.g. the leader moved the start once everyone had
        // buffered): keep decoding into, and streaming from, the same buffer.
        install(RenderSourceKind::stream, activeFeeder_->buffer().get(), true);
    } else {
        applySource(true);
    }
}

void Player::setChannel(SpeakerChannel channel, int64_t leaderTime) {
    channel_ = channel;
    renderer_->setChannelMix(ChannelMix::forChannel(channel), leaderTime);
}

void Player::add(std::shared_ptr<const MediaFile> file) {
    const uint64_t id = file->id;
    files_[id] = std::move(file);
    if (playback_.source == id) applySource(true);
}

std::shared_ptr<const MediaFile> Player::file(uint64_t id) const {
    const auto file = files_.find(id);
    return file == files_.end() ? nullptr : file->second;
}

void Player::setRemoteStream(const Ref<StreamBuffer>& buffer, uint64_t source) {
    if (remoteBuffer_) retire(remoteBuffer_);
    remoteBuffer_ = buffer;
    remoteStreamSource_ = source;
    if (playback_.source == source && files_.count(source) == 0) {
        // Playback position is derived from time, not from the buffer, so swapping the buffer
        // under a playing renderer needs no repositioning.
        applySource(false);
    }
}

void Player::clearRemoteStream() {
    if (remoteBuffer_) retire(remoteBuffer_);
    remoteBuffer_ = Ref<StreamBuffer>();
    remoteStreamSource_.reset();
    applySource(false);
}

bool Player::isWaitingForAudio() const {
    return playback_.isPlaying() && !isGeneratedSource(playback_.source) && files_.count(playback_.source) == 0
        && remoteStreamSource_ != playback_.source;
}

void Player::releaseRetiredBuffers() {
    const auto now = std::chrono::steady_clock::now();
    while (!retired_.empty() && now - retired_.front().first >= kRetireDelay) {
        retired_.pop_front();
    }
}

void Player::retire(const Ref<StreamBuffer>& buffer) {
    retired_.emplace_back(std::chrono::steady_clock::now(), buffer);
}

bool Player::canKeepStream(const PlaybackState& previous) const {
    int64_t heard = 0;
    if (!activeFeeder_ || previous.source != playback_.source || !playback_.isPlaying() || !position_->heardMediaTime(hostClockNanoseconds(), heard)) {
        return false;
    }
    const StreamBuffer& buffer = *activeFeeder_->buffer();
    const int64_t time = std::max({playback_.startMediaTime, heard, int64_t{0}});
    const auto frame = static_cast<int64_t>(std::floor(static_cast<double>(time) * buffer.sampleRate() / 1e9));
    // The decoder only moves forward from where the buffer ends, so the new position must lie
    // within what the buffer holds or will soon hold.
    const FrameRange range = buffer.validRange();
    return frame >= range.lowerBound && frame <= range.upperBound + static_cast<int64_t>(buffer.sampleRate());
}

void Player::applySource(bool reposition) {
    const bool hadOutgoingStream = activeFeeder_ != nullptr;
    stopStream();
    RenderSourceKind kind = RenderSourceKind::silence;
    Ref<StreamBuffer> stream;
    if (!playback_.isPlaying()) {
        kind = RenderSourceKind::silence;
    } else if (playback_.source == kPlaybackSourceMetronome) {
        kind = RenderSourceKind::metronome;
    } else if (playback_.source == kPlaybackSourceChannelTest) {
        kind = RenderSourceKind::channelTest;
    } else if (const auto file = files_.find(playback_.source); file != files_.end()) {
        stream = startStream(file->second);
        kind = stream ? RenderSourceKind::stream : RenderSourceKind::silence;
    } else if (remoteBuffer_ && remoteStreamSource_ == playback_.source) {
        kind = RenderSourceKind::stream;
        stream = remoteBuffer_;
    }
    if (hadOutgoingStream && !activeFeeder_ && onOutgoingStream) onOutgoingStream(std::nullopt);
    install(kind, stream.get(), reposition);
}

void Player::install(RenderSourceKind kind, StreamBuffer* stream, bool reposition) {
    if (reposition) ++generation_;
    renderer_->setPlayback(playback_, generation_, kind, stream);
}

/// Starts decoding `file` from just before the position being heard now (or from the scheduled
/// start, if that is still ahead). Returns null until the clock map is known.
Ref<StreamBuffer> Player::startStream(const std::shared_ptr<const MediaFile>& file) {
    int64_t heard = 0;
    if (!position_->heardMediaTime(hostClockNanoseconds(), heard) || !(file->sampleRate > 0)) return Ref<StreamBuffer>();
    const double rate = file->sampleRate;
    // A little slack before "now" so a late join loses nothing to the time decoding takes.
    const int64_t startTime = std::max({playback_.startMediaTime, heard - int64_t{100'000'000}, int64_t{0}});
    const auto startFrame = static_cast<int64_t>(std::floor(static_cast<double>(startTime) * rate / 1e9));

    // Room for the read-ahead plus a few seconds behind playback, which the decoder may
    // overwrite only once they have been heard.
    Ref<StreamBuffer> buffer = Ref<StreamBuffer>::adopt(StreamBuffer::create(rate, static_cast<int>((kStreamAheadDuration + 4) * rate), startFrame));
    FileStreamFeeder::Configuration configuration;
    configuration.aheadDuration = kStreamAheadDuration;
    auto feeder = std::make_unique<FileStreamFeeder>(file, buffer, startFrame, position_, configuration);
    FileStreamFeeder* raw = feeder.get();
    feeder->onStall = [this, raw] {
        post_([this, raw] {
            // The feeder fell behind (the process was starved): start over from where playback is.
            if (activeFeeder_.get() == raw) applySource(true);
        });
    };
    activeFeeder_ = std::move(feeder);
    activeFeeder_->start();
    if (onOutgoingStream) onOutgoingStream(OutgoingStream{randomStreamID(), file->id, buffer, position_});
    return buffer;
}

void Player::stopStream() {
    if (!activeFeeder_) return;
    activeFeeder_->stop();
    retire(activeFeeder_->buffer());
    activeFeeder_.reset();
}

}  // namespace asyncmusic
