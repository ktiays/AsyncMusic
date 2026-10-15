//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/playback.hpp"

namespace asyncmusic {

void PlaybackPosition::setPlayback(const PlaybackState& playback) {
    std::lock_guard<std::mutex> lock(mutex_);
    playback_ = playback;
}

PlaybackState PlaybackPosition::playback() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return playback_;
}

void PlaybackPosition::setClockMap(const ClockMap& map) {
    std::lock_guard<std::mutex> lock(mutex_);
    clockMap_ = map;
    hasClockMap_ = true;
}

void PlaybackPosition::clearClockMap() {
    std::lock_guard<std::mutex> lock(mutex_);
    hasClockMap_ = false;
}

bool PlaybackPosition::hasClockMap() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hasClockMap_;
}

ClockMap PlaybackPosition::clockMap() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clockMap_;
}

bool PlaybackPosition::heardMediaTime(int64_t now, int64_t& mediaTime) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasClockMap_) return false;
    mediaTime = playback_.mediaTimeAtLeaderTime(clockMap_.leaderTimeFromLocal(now));
    return true;
}

}  // namespace asyncmusic
