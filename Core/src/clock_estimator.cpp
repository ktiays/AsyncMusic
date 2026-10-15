//
//  Created by ktiays on 2026/10/16.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

#include "asyncmusic/clock_estimator.hpp"

#include <algorithm>
#include <cmath>

namespace asyncmusic {

ClockEstimator::ClockEstimator(ClockEstimatorConfiguration configuration) : configuration_(configuration) {}

void ClockEstimator::reset() {
    points_.clear();
    roundTripHistory_.clear();
    hasMap_ = false;
    map_ = {};
    residualRMS_ = 0;
    consecutiveOutliers_ = 0;
}

int64_t ClockEstimator::minimumRoundTrip() const {
    if (roundTripHistory_.empty()) return 0;
    return *std::min_element(roundTripHistory_.begin(), roundTripHistory_.end());
}

ClockVerdict ClockEstimator::addRound(const ClockProbe* probes, size_t count) {
    if (probes == nullptr || count == 0) return ClockVerdict::noReplies;
    const ClockProbe* best = probes;
    for (size_t i = 1; i < count; ++i) {
        if (probes[i].roundTrip() < best->roundTrip()) best = probes + i;
    }
    return add(*best);
}

ClockVerdict ClockEstimator::add(const ClockProbe& probe) {
    roundTripHistory_.push_back(probe.roundTrip());
    const size_t floorWindow = static_cast<size_t>(configuration_.roundTripFloorWindow);
    if (roundTripHistory_.size() > floorWindow) {
        roundTripHistory_.erase(roundTripHistory_.begin(), roundTripHistory_.end() - floorWindow);
    }
    const int64_t floor = minimumRoundTrip();
    const int64_t limit = static_cast<int64_t>(static_cast<double>(floor) * configuration_.roundTripTolerance) + configuration_.roundTripSlack;
    if (probe.roundTrip() > limit) {
        rejectedCount_ += 1;
        return ClockVerdict::slowRoundTrip;
    }

    ClockVerdict verdict = ClockVerdict::accepted;
    if (hasMap_ && points_.size() >= 4) {
        const double residual = static_cast<double>(probe.offset() - map_.offsetAtLocal(probe.localMidpoint()));
        const double threshold = std::max(configuration_.outlierFloor, configuration_.outlierSigma * residualRMS_);
        if (std::abs(residual) > threshold) {
            consecutiveOutliers_ += 1;
            if (consecutiveOutliers_ < configuration_.resetAfterOutliers) {
                rejectedCount_ += 1;
                return ClockVerdict::outlier;
            }
            // The leader's timeline genuinely moved; start over from this point.
            std::vector<int64_t> history = roundTripHistory_;
            reset();
            roundTripHistory_ = std::move(history);
            verdict = ClockVerdict::reset;
        }
    }
    consecutiveOutliers_ = 0;

    points_.push_back(Point{probe.localMidpoint(), probe.offset(), probe.roundTrip()});
    const size_t window = static_cast<size_t>(configuration_.windowSize);
    if (points_.size() > window) {
        points_.erase(points_.begin(), points_.end() - window);
    }
    acceptedCount_ += 1;
    refit();
    return verdict;
}

void ClockEstimator::refit() {
    if (points_.empty()) {
        hasMap_ = false;
        residualRMS_ = 0;
        return;
    }
    const Point& first = points_.front();
    const Point& last = points_.back();
    const size_t count = points_.size();

    // Work relative to the newest point so the doubles stay small and precise.
    std::vector<double> xs(count), ys(count), weights(count);
    int64_t fastest = points_[0].roundTrip;
    for (const Point& point : points_) fastest = std::min(fastest, point.roundTrip);
    double totalWeight = 0, meanX = 0, meanY = 0;
    for (size_t i = 0; i < count; ++i) {
        xs[i] = static_cast<double>(points_[i].local - last.local);
        ys[i] = static_cast<double>(points_[i].offset - last.offset);
        // A point's asymmetry error is bounded by half of its excess round trip, so points
        // measured closer to the fastest path get more say in the fit.
        const double bound = static_cast<double>(points_[i].roundTrip - fastest) / 2 + configuration_.weightFloor;
        weights[i] = 1 / (bound * bound);
        totalWeight += weights[i];
        meanX += weights[i] * xs[i];
        meanY += weights[i] * ys[i];
    }
    meanX /= totalWeight;
    meanY /= totalWeight;

    double slope = 0;
    if (count >= 3 && last.local - first.local >= configuration_.minimumSkewSpan) {
        double sxx = 0, sxy = 0;
        for (size_t i = 0; i < count; ++i) {
            sxx += weights[i] * (xs[i] - meanX) * (xs[i] - meanX);
            sxy += weights[i] * (xs[i] - meanX) * (ys[i] - meanY);
        }
        if (sxx > 0) {
            slope = std::clamp(sxy / sxx, -configuration_.maximumSkew, configuration_.maximumSkew);
        }
    }

    double squaredResiduals = 0;
    for (size_t i = 0; i < count; ++i) {
        const double residual = ys[i] - (meanY + slope * (xs[i] - meanX));
        squaredResiduals += residual * residual;
    }
    residualRMS_ = std::sqrt(squaredResiduals / static_cast<double>(count));

    // The least-squares line passes through the weighted centroid, so anchor the map there.
    map_ = ClockMap{
        last.local + static_cast<int64_t>(std::llround(meanX)),
        last.offset + static_cast<int64_t>(std::llround(meanY)),
        slope,
    };
    hasMap_ = true;
}

}  // namespace asyncmusic
