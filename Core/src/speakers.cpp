//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/speakers.hpp"

namespace asyncmusic {

int speakerLayoutChannelCount(SpeakerLayout layout) {
    switch (layout) {
    case SpeakerLayout::everywhere:
        return 0;
    case SpeakerLayout::stereoPair:
        return 2;
    }
    return 0;
}

SpeakerChannel speakerLayoutChannel(SpeakerLayout layout, int index) {
    switch (layout) {
    case SpeakerLayout::everywhere:
        break;
    case SpeakerLayout::stereoPair:
        if (index == 0) return SpeakerChannel::left;
        if (index == 1) return SpeakerChannel::right;
        break;
    }
    return SpeakerChannel::full;
}

ChannelMix ChannelMix::forChannel(SpeakerChannel channel) {
    switch (channel) {
    case SpeakerChannel::full:
        return ChannelMix{};
    case SpeakerChannel::left:
        return ChannelMix{1, 0, 1, 0};
    case SpeakerChannel::right:
        return ChannelMix{0, 1, 0, 1};
    }
    return ChannelMix{};
}

}  // namespace asyncmusic
