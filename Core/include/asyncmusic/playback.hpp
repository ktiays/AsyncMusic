//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstdint>
#include <mutex>

#include "clock.hpp"
#include "config.hpp"
#include "ref_counted.hpp"

AM_ASSUME_NONNULL_BEGIN

namespace asyncmusic {

/// Identifies the audio source; `kPlaybackSourceNone` means stopped. Files use a content hash,
/// which is never one of the reserved values.
constexpr uint64_t kPlaybackSourceNone = 0;
constexpr uint64_t kPlaybackSourceMetronome = 1;

/// What the group should be playing, expressed on the leader's clock.
///
/// Media time advances one-for-one with leader time from the anchor, so every device can work
/// out which instant of the source belongs to any moment, whatever its own sample rate:
///
///     mediaTime(leaderTime) = startMediaTime + (leaderTime - startLeaderTime)
struct PlaybackState {
    uint64_t source = kPlaybackSourceNone;
    /// Leader host time (nanoseconds) at which `startMediaTime` is heard.
    int64_t startLeaderTime = 0;
    /// Position in the source (nanoseconds) heard at `startLeaderTime`. Negative values are
    /// allowed and simply mean silence until the source reaches zero.
    int64_t startMediaTime = 0;

    static PlaybackState stopped() { return PlaybackState{}; }

    bool isPlaying() const { return source != kPlaybackSourceNone; }

    int64_t mediaTimeAtLeaderTime(int64_t leaderTime) const { return startMediaTime + (leaderTime - startLeaderTime); }

    bool operator==(const PlaybackState& other) const {
        return source == other.source && startLeaderTime == other.startLeaderTime && startMediaTime == other.startMediaTime;
    }
    bool operator!=(const PlaybackState& other) const { return !(*this == other); }
};

class PlaybackPosition;
}  // namespace asyncmusic

void asyncmusic_playback_position_retain(asyncmusic::PlaybackPosition* object);
void asyncmusic_playback_position_release(asyncmusic::PlaybackPosition* object);

namespace asyncmusic {

/// The current playback state and clock map, readable from any thread.
///
/// Decoders and stream senders pace themselves on the position being heard. Reading it through
/// this shared object, rather than a copy taken when a stream started, lets the leader
/// reschedule the start (adaptive start) without restarting a stream already being buffered.
class PlaybackPosition final : public RefCounted {
public:
    static PlaybackPosition* create() SWIFT_RETURNS_RETAINED { return new PlaybackPosition(); }

    void setPlayback(const PlaybackState& playback);
    PlaybackState playback() const;

    void setClockMap(const ClockMap& map);
    void clearClockMap();
    bool hasClockMap() const;
    ClockMap clockMap() const;

    /// Writes the media time (nanoseconds) heard at local host time `now` to `mediaTime`.
    /// Returns false while the clock map is unknown.
    bool heardMediaTime(int64_t now, int64_t& mediaTime) const;

private:
    PlaybackPosition() = default;

    mutable std::mutex mutex_;
    PlaybackState playback_;
    ClockMap clockMap_;
    bool hasClockMap_ = false;
} SWIFT_SHARED_REFERENCE(asyncmusic_playback_position_retain, asyncmusic_playback_position_release);

}  // namespace asyncmusic

inline void asyncmusic_playback_position_retain(asyncmusic::PlaybackPosition* object) { object->retain(); }
inline void asyncmusic_playback_position_release(asyncmusic::PlaybackPosition* object) { object->release(); }

AM_ASSUME_NONNULL_END
