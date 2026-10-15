//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "clock.hpp"
#include "config.hpp"

AM_ASSUME_NONNULL_BEGIN

namespace asyncmusic {

struct ClockEstimatorConfiguration {
    /// Number of accepted rounds the line is fitted over.
    int windowSize = 64;
    /// Number of recent rounds (accepted or not) the round-trip floor is taken from.
    int roundTripFloorWindow = 32;
    /// A round is dropped when its best round trip exceeds `floor * tolerance + slack`.
    double roundTripTolerance = 1.5;
    int64_t roundTripSlack = 1'000'000;
    /// Skew is only estimated once the window spans this long; shorter spans give a slope
    /// dominated by noise, so the offset is treated as constant until then.
    int64_t minimumSkewSpan = 10'000'000'000;
    /// Real crystals are within ~100 ppm of each other; anything beyond this is noise.
    double maximumSkew = 500e-6;
    /// A round whose offset misses the fitted line by more than
    /// `max(outlierFloor, outlierSigma * residualRMS)` nanoseconds is an outlier.
    double outlierFloor = 1'000'000;
    double outlierSigma = 5;
    /// This many outliers in a row means the leader's timeline itself moved, so start over.
    int resetAfterOutliers = 5;
    /// Added to each point's asymmetry bound before weighting, in nanoseconds, so that the
    /// fastest point does not get infinite weight.
    double weightFloor = 100'000;
};

enum class AM_CLOSED_ENUM ClockVerdict : uint8_t {
    accepted,
    noReplies,
    slowRoundTrip,
    outlier,
    reset,
};

/// Turns rounds of noisy probes into a `ClockMap`.
///
/// Network delay only ever adds time, and Wi-Fi adds a lot of it one-sidedly (power save,
/// retransmits, queueing). So rather than averaging everything, each round keeps only its
/// fastest probe, rounds that were slow compared with the recent best are dropped, and a
/// weighted least-squares line through the survivors gives both the offset and the skew.
class ClockEstimator {
public:
    struct Point {
        int64_t local = 0;
        int64_t offset = 0;
        int64_t roundTrip = 0;
    };

    explicit ClockEstimator(ClockEstimatorConfiguration configuration = {});

    /// Feeds one round of probes. Only the fastest one is used.
    ClockVerdict addRound(const ClockProbe* AM_NULLABLE probes, size_t count);
    ClockVerdict add(const ClockProbe& probe);
    void reset();

    bool hasMap() const { return hasMap_; }
    /// The current mapping; only meaningful when `hasMap()`.
    ClockMap map() const { return map_; }
    /// Root-mean-square distance of the accepted points from the fitted line, in nanoseconds.
    double residualRMS() const { return residualRMS_; }
    int acceptedCount() const { return acceptedCount_; }
    int rejectedCount() const { return rejectedCount_; }
    int windowCount() const { return static_cast<int>(points_.size()); }
    /// Lowest round trip seen recently (0 if none); half of it bounds the asymmetry error.
    int64_t minimumRoundTrip() const;
    const ClockEstimatorConfiguration& configuration() const { return configuration_; }

private:
    void refit();

    ClockEstimatorConfiguration configuration_;
    std::vector<Point> points_;
    std::vector<int64_t> roundTripHistory_;
    ClockMap map_;
    bool hasMap_ = false;
    double residualRMS_ = 0;
    int acceptedCount_ = 0;
    int rejectedCount_ = 0;
    int consecutiveOutliers_ = 0;
};

}  // namespace asyncmusic

AM_ASSUME_NONNULL_END
