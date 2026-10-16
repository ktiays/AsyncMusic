//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>

#include "clock.hpp"
#include "playback.hpp"
#include "ref_counted.hpp"
#include "renderer.hpp"
#include "speakers.hpp"
#include "stream_buffer.hpp"

namespace asyncmusic {

/// Keeps a `SyncedRenderer` fed with the clock map, the playback state and what to play: the
/// platform-independent part of the Apple app's `SyncedPlayer`. The platform owns the audio
/// output and calls `renderer().render(...)` from it, and sets the presentation delay.
///
/// Not thread-safe: call it from one thread (the session's), except for `renderer()`, whose
/// `render` belongs to the audio thread.
class Player {
public:
    Player();

    SyncedRenderer& renderer() { return *renderer_; }
    /// The heard position, readable from any thread.
    PlaybackPosition& position() { return *position_; }

    void setClockMap(const std::optional<ClockMap>& map);
    const std::optional<ClockMap>& clockMap() const { return clockMap_; }

    void setPlayback(const PlaybackState& playback);
    const PlaybackState& playback() const { return playback_; }

    /// Plays only `channel` of the source (on all of this device's outputs) from leader time
    /// `leaderTime` on, as heard; the default switches right away.
    void setChannel(SpeakerChannel channel, int64_t leaderTime = std::numeric_limits<int64_t>::min());
    SpeakerChannel channel() const { return channel_; }

    /// Plays `source` from `buffer`, which the leader's stream fills. Replaces any previous
    /// remote stream.
    void setRemoteStream(const Ref<StreamBuffer>& buffer, uint64_t source);
    std::optional<uint64_t> remoteStreamSource() const { return remoteStreamSource_; }
    /// Forgets the remote stream (e.g. after leaving a leader).
    void clearRemoteStream();

    /// True when a file is playing that this device has no audio for.
    bool isWaitingForAudio() const;

    /// Releases buffers swapped out long enough ago; call about once a second.
    void releaseRetiredBuffers();

    RenderStats stats() const { return renderer_->latestStats(); }

private:
    void applySource(bool reposition);
    void retire(const Ref<StreamBuffer>& buffer);

    Ref<SyncedRenderer> renderer_;
    Ref<PlaybackPosition> position_;
    std::optional<ClockMap> clockMap_;
    PlaybackState playback_;
    SpeakerChannel channel_ = SpeakerChannel::full;
    int generation_ = 0;
    Ref<StreamBuffer> remoteBuffer_;
    std::optional<uint64_t> remoteStreamSource_;
    /// Buffers recently swapped out, oldest first. The renderer holds them unretained and may
    /// keep using one until its next successful try-lock, so they are released after a delay.
    std::deque<std::pair<std::chrono::steady_clock::time_point, Ref<StreamBuffer>>> retired_;
};

/// Whether `source` is generated on the device (metronome, channel test) rather than a file.
inline bool isGeneratedSource(uint64_t source) {
    return source == kPlaybackSourceMetronome || source == kPlaybackSourceChannelTest;
}

}  // namespace asyncmusic
