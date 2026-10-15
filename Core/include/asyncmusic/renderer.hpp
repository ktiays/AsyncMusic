//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstdint>
#include <mutex>

#include "clock.hpp"
#include "config.hpp"
#include "playback.hpp"
#include "ref_counted.hpp"
#include "stream_buffer.hpp"

AM_ASSUME_NONNULL_BEGIN

namespace asyncmusic {

enum class AM_CLOSED_ENUM RenderSourceKind : uint8_t {
    silence,
    metronome,
    stream,
};

/// Snapshot of what the render thread is doing, for diagnostics.
struct RenderStats {
    bool hostTimeValid = true;
    bool isPlaying = false;
    /// Where the output should be minus where it is, in microseconds (before correction).
    double errorMicroseconds = 0;
    /// Current playback-rate correction, in parts per million.
    double correctionPPM = 0;
    /// Number of times playback was repositioned instead of smoothly corrected.
    int jumps = 0;
    /// Source position currently being output, in seconds.
    double mediaSeconds = 0;
    /// How far the stream buffer reaches beyond the current position, in seconds.
    double bufferedSeconds = 0;
    /// Times stream data ran out mid-playback (including once at the end of a file).
    int gaps = 0;
};

struct RendererConfiguration {
    /// Proportional time constant of the rate controller, in seconds. The integral time
    /// constant is four times this, which makes the loop critically damped.
    double correctionTimeConstant = 1.0;
    /// 1000 ppm is a 1.7 cent pitch change, below what anyone can hear.
    double maximumCorrection = 1000e-6;
    /// Errors beyond this many seconds reposition the read head instead.
    double jumpThreshold = 0.010;
    /// Length of fades after repositioning and around stream gaps, in seconds.
    double fadeDuration = 0.005;
};

/// A click track computed directly from media time, so every device produces exactly the same
/// signal at the same media instant regardless of sample rate. Sharp attacks make any timing
/// difference between devices easy to hear as a flam.
float metronomeSample(double seconds);

class SyncedRenderer;
}  // namespace asyncmusic

void asyncmusic_renderer_retain(asyncmusic::SyncedRenderer* object);
void asyncmusic_renderer_release(asyncmusic::SyncedRenderer* object);

namespace asyncmusic {

/// Works out, for every render cycle, which part of the source must be heard when that buffer
/// reaches the speaker, and plays it.
///
/// Rather than starting playback once and hoping it stays aligned, each cycle maps the buffer's
/// presentation time onto the leader's clock and from there onto media time. Small differences
/// between that target and the running read position (clock-map updates, DAC crystal drift)
/// are absorbed by a PI controller nudging the playback rate by up to `maximumCorrection`; the
/// integral term learns the DAC's steady rate error so it is tracked without a lag. Large
/// differences (start, seek, a big re-estimate) reposition the read head directly.
///
/// `render` runs on the real-time audio thread: it never blocks, allocates or touches reference
/// counts. Control state comes in through the setters, which the render thread picks up with a
/// try-lock, otherwise keeping its previous copy.
class SyncedRenderer final : public RefCounted {
public:
    static SyncedRenderer* create() SWIFT_RETURNS_RETAINED { return new SyncedRenderer(RendererConfiguration{}); }
    static SyncedRenderer* create(const RendererConfiguration& configuration) SWIFT_RETURNS_RETAINED {
        return new SyncedRenderer(configuration);
    }

    // Control (any thread)

    void setClockMap(const ClockMap& map);
    void clearClockMap();
    /// Host time from the render timestamp to the sound leaving the device (plus any manual
    /// offset), in nanoseconds. Platforms whose timestamps already give presentation time
    /// pass the remaining delay only.
    void setPresentationDelay(int64_t nanoseconds);
    /// Installs what to play. A new `generation` repositions the read head; the same one keeps
    /// it (e.g. when only the stream buffer is swapped). The renderer does not retain
    /// `stream`: the caller must keep it alive until the render thread has picked up a later
    /// state, in practice by releasing replaced buffers only after a short delay.
    void setPlayback(const PlaybackState& playback, int generation, RenderSourceKind kind, StreamBuffer* AM_NULLABLE stream);

    RenderStats latestStats() const;

    // Audio thread

    /// Fills `frameCount` frames of non-interleaved stereo. `hostTime` is when this buffer's
    /// first frame is presented, minus `setPresentationDelay`, on the host clock.
    void render(bool hasHostTime, int64_t hostTime, int frameCount, double outputRate, float* left, float* right);

private:
    explicit SyncedRenderer(const RendererConfiguration& configuration);

    struct ControlState {
        ClockMap clockMap;
        bool hasClockMap = false;
        PlaybackState playback;
        int generation = 0;
        RenderSourceKind kind = RenderSourceKind::silence;
        StreamBuffer* AM_NULLABLE stream = nullptr;
        int64_t presentationDelay = 0;
    };

    void renderStream(const StreamBuffer& buffer, double sourceRate, double increment, int frameCount, double outputRate, float* left, float* right);
    void publishStats();

    const RendererConfiguration configuration_;

    mutable std::mutex controlMutex_;
    ControlState shared_;
    mutable std::mutex statsMutex_;
    RenderStats published_;

    // Render-thread-only state.
    ControlState current_;
    double position_ = 0;
    int lastGeneration_ = -1;
    bool wasAudible_ = false;
    int fadeRemaining_ = 0;
    /// True while the stream has no data for the playing position (but should have).
    bool inStreamGap_ = false;
    /// Integral term of the rate controller: the learned steady-state rate error.
    double integral_ = 0;
    RenderStats stats_;
} SWIFT_SHARED_REFERENCE(asyncmusic_renderer_retain, asyncmusic_renderer_release);

}  // namespace asyncmusic

inline void asyncmusic_renderer_retain(asyncmusic::SyncedRenderer* object) { object->retain(); }
inline void asyncmusic_renderer_release(asyncmusic::SyncedRenderer* object) { object->release(); }

AM_ASSUME_NONNULL_END
