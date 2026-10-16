//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/player.hpp"

namespace asyncmusic {

namespace {
/// How long a swapped-out buffer is kept for the render thread.
constexpr auto kRetireDelay = std::chrono::seconds(2);
}  // namespace

Player::Player()
    : renderer_(Ref<SyncedRenderer>::adopt(SyncedRenderer::create())), position_(Ref<PlaybackPosition>::adopt(PlaybackPosition::create())) {}

void Player::setClockMap(const std::optional<ClockMap>& map) {
    clockMap_ = map;
    if (map) {
        renderer_->setClockMap(*map);
        position_->setClockMap(*map);
    } else {
        renderer_->clearClockMap();
        position_->clearClockMap();
    }
}

void Player::setPlayback(const PlaybackState& playback) {
    // Re-applying the same state would needlessly reposition the read head.
    if (playback == playback_) return;
    playback_ = playback;
    position_->setPlayback(playback);
    applySource(true);
}

void Player::setChannel(SpeakerChannel channel, int64_t leaderTime) {
    channel_ = channel;
    renderer_->setChannelMix(ChannelMix::forChannel(channel), leaderTime);
}

void Player::setRemoteStream(const Ref<StreamBuffer>& buffer, uint64_t source) {
    if (remoteBuffer_) retire(remoteBuffer_);
    remoteBuffer_ = buffer;
    remoteStreamSource_ = source;
    if (playback_.source == source) {
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
    return playback_.isPlaying() && !isGeneratedSource(playback_.source) && remoteStreamSource_ != playback_.source;
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

void Player::applySource(bool reposition) {
    if (reposition) ++generation_;
    RenderSourceKind kind = RenderSourceKind::silence;
    StreamBuffer* stream = nullptr;
    if (!playback_.isPlaying()) {
        kind = RenderSourceKind::silence;
    } else if (playback_.source == kPlaybackSourceMetronome) {
        kind = RenderSourceKind::metronome;
    } else if (playback_.source == kPlaybackSourceChannelTest) {
        kind = RenderSourceKind::channelTest;
    } else if (remoteBuffer_ && remoteStreamSource_ == playback_.source) {
        kind = RenderSourceKind::stream;
        stream = remoteBuffer_.get();
    }
    renderer_->setPlayback(playback_, generation_, kind, stream);
}

}  // namespace asyncmusic
