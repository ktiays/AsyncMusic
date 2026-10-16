//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstdint>

#include "config.hpp"

AM_ASSUME_NONNULL_BEGIN

namespace asyncmusic {

/// How the group spreads the music over its devices.
///
/// A layout names a set of channels, each played by one device (see `speakerLayoutChannel`).
/// Devices without a channel, and every device while the layout is incomplete (a channel has
/// no device, or its device dropped out), play the full mix, so nothing goes missing.
/// Surround layouts (5.1, 7.1) slot in here once the stream carries more than two channels.
enum class AM_CLOSED_ENUM SpeakerLayout : uint8_t {
    /// Every device plays the full stereo mix.
    everywhere = 0,
    /// Two devices: one plays the left channel, the other the right.
    stereoPair = 1,
};

/// What a single device plays.
enum class AM_CLOSED_ENUM SpeakerChannel : uint8_t {
    /// The full stereo mix, as is.
    full = 0,
    left = 1,
    right = 2,
};

/// Number of channels (and so devices) a layout splits the music into; 0 for `everywhere`.
int speakerLayoutChannelCount(SpeakerLayout layout);
/// The `index`th channel of `layout`, in display order (left before right).
SpeakerChannel speakerLayoutChannel(SpeakerLayout layout, int index);

/// Largest valid raw value, for validating values from the wire.
constexpr uint8_t kSpeakerLayoutMaximum = static_cast<uint8_t>(SpeakerLayout::stereoPair);
constexpr uint8_t kSpeakerChannelMaximum = static_cast<uint8_t>(SpeakerChannel::right);

/// A 2x2 matrix from the source's stereo channels to the device's output channels:
///
///     outLeft  = leftFromLeft  * left + leftFromRight  * right
///     outRight = rightFromLeft * left + rightFromRight * right
///
/// A device playing one channel of a stereo pair puts that channel on both of its outputs, so
/// a device with built-in stereo speakers still sounds like one point source.
struct ChannelMix {
    float leftFromLeft = 1;
    float leftFromRight = 0;
    float rightFromLeft = 0;
    float rightFromRight = 1;

    static ChannelMix forChannel(SpeakerChannel channel);

    bool isIdentity() const { return leftFromLeft == 1 && leftFromRight == 0 && rightFromLeft == 0 && rightFromRight == 1; }

    bool operator==(const ChannelMix& other) const {
        return leftFromLeft == other.leftFromLeft && leftFromRight == other.leftFromRight && rightFromLeft == other.rightFromLeft
            && rightFromRight == other.rightFromRight;
    }
    bool operator!=(const ChannelMix& other) const { return !(*this == other); }
};

}  // namespace asyncmusic

AM_ASSUME_NONNULL_END
