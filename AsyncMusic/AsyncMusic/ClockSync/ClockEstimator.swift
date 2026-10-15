//
//  Created by ktiays on 2026/10/15.
//  Copyright (c) 2026 ktiays. All rights reserved.
//

/// Turns rounds of noisy probes into a `ClockMap`.
///
/// Network delay only ever adds time, and Wi-Fi adds a lot of it one-sidedly (power save,
/// retransmits, queueing). So rather than averaging everything, each round keeps only its
/// fastest probe, rounds that were slow compared with the recent best are dropped, and a
/// least-squares line through the survivors gives both the offset and the skew.
nonisolated struct ClockEstimator: Sendable {
    struct Configuration: Sendable {
        /// Number of accepted rounds the line is fitted over.
        var windowSize = 64
        /// Number of recent rounds (accepted or not) the round-trip floor is taken from.
        var roundTripFloorWindow = 32
        /// A round is dropped when its best round trip exceeds `floor * tolerance + slack`.
        var roundTripTolerance = 1.5
        var roundTripSlack: Int64 = 1_000_000
        /// Skew is only estimated once the window spans this long; shorter spans give a
        /// slope dominated by noise, so the offset is treated as constant until then.
        var minimumSkewSpan: Int64 = 10_000_000_000
        /// Real crystals are within ~100 ppm of each other; anything beyond this is noise.
        var maximumSkew = 500e-6
        /// A round whose offset misses the fitted line by more than
        /// `max(outlierFloor, outlierSigma * residualRMS)` nanoseconds is an outlier.
        var outlierFloor: Double = 1_000_000
        var outlierSigma = 5.0
        /// This many outliers in a row means the leader's timeline itself moved, so start over.
        var resetAfterOutliers = 5
        /// Added to each point's asymmetry bound before weighting, in nanoseconds, so that the
        /// fastest point does not get infinite weight.
        var weightFloor: Double = 100_000
    }

    enum Verdict: Sendable, Equatable {
        case accepted
        case noReplies
        case slowRoundTrip
        case outlier
        case reset
    }

    struct Point: Sendable, Equatable {
        var local: Int64
        var offset: Int64
        var roundTrip: Int64
    }

    let configuration: Configuration

    private(set) var points: [Point] = []
    private(set) var map: ClockMap?
    /// Root-mean-square distance of the accepted points from the fitted line, in nanoseconds.
    private(set) var residualRMS: Double = 0
    private(set) var acceptedCount = 0
    private(set) var rejectedCount = 0

    private var roundTripHistory: [Int64] = []
    private var consecutiveOutliers = 0

    init(configuration: Configuration = Configuration()) {
        self.configuration = configuration
    }

    /// Lowest round trip seen recently; half of it bounds the error from path asymmetry.
    var minimumRoundTrip: Int64? { roundTripHistory.min() }

    mutating func reset() {
        points = []
        map = nil
        residualRMS = 0
        roundTripHistory = []
        consecutiveOutliers = 0
    }

    /// Feeds one round of probes. Only the fastest one is used.
    @discardableResult
    mutating func addRound(_ probes: [ClockProbe]) -> Verdict {
        guard let best = probes.min(by: { $0.roundTrip < $1.roundTrip }) else {
            return .noReplies
        }
        return add(best)
    }

    @discardableResult
    mutating func add(_ probe: ClockProbe) -> Verdict {
        roundTripHistory.append(probe.roundTrip)
        if roundTripHistory.count > configuration.roundTripFloorWindow {
            roundTripHistory.removeFirst(roundTripHistory.count - configuration.roundTripFloorWindow)
        }
        let floor = roundTripHistory.min() ?? probe.roundTrip
        let limit = Int64(Double(floor) * configuration.roundTripTolerance) + configuration.roundTripSlack
        guard probe.roundTrip <= limit else {
            rejectedCount += 1
            return .slowRoundTrip
        }

        var verdict = Verdict.accepted
        if let map, points.count >= 4 {
            let residual = Double(probe.offset - map.offset(atLocal: probe.localMidpoint))
            let threshold = max(configuration.outlierFloor, configuration.outlierSigma * residualRMS)
            if abs(residual) > threshold {
                consecutiveOutliers += 1
                guard consecutiveOutliers >= configuration.resetAfterOutliers else {
                    rejectedCount += 1
                    return .outlier
                }
                let history = roundTripHistory
                reset()
                roundTripHistory = history
                verdict = .reset
            }
        }
        consecutiveOutliers = 0

        points.append(Point(local: probe.localMidpoint, offset: probe.offset, roundTrip: probe.roundTrip))
        if points.count > configuration.windowSize {
            points.removeFirst(points.count - configuration.windowSize)
        }
        acceptedCount += 1
        refit()
        return verdict
    }

    private mutating func refit() {
        guard let first = points.first, let last = points.last else {
            map = nil
            residualRMS = 0
            return
        }

        // Work relative to the newest point so the doubles stay small and precise.
        let xs = points.map { Double($0.local - last.local) }
        let ys = points.map { Double($0.offset - last.offset) }

        // A point's asymmetry error is bounded by half of its excess round trip, so points
        // measured closer to the fastest path get more say in the fit.
        let fastest = points.map(\.roundTrip).min() ?? 0
        let weights = points.map { point -> Double in
            let bound = Double(point.roundTrip - fastest) / 2 + configuration.weightFloor
            return 1 / (bound * bound)
        }
        let totalWeight = weights.reduce(0, +)
        let meanX = zip(weights, xs).reduce(0) { $0 + $1.0 * $1.1 } / totalWeight
        let meanY = zip(weights, ys).reduce(0) { $0 + $1.0 * $1.1 } / totalWeight

        var slope = 0.0
        if points.count >= 3, last.local - first.local >= configuration.minimumSkewSpan {
            var sxx = 0.0
            var sxy = 0.0
            for ((x, y), weight) in zip(zip(xs, ys), weights) {
                sxx += weight * (x - meanX) * (x - meanX)
                sxy += weight * (x - meanX) * (y - meanY)
            }
            if sxx > 0 {
                slope = min(max(sxy / sxx, -configuration.maximumSkew), configuration.maximumSkew)
            }
        }

        var squaredResiduals = 0.0
        for (x, y) in zip(xs, ys) {
            let residual = y - (meanY + slope * (x - meanX))
            squaredResiduals += residual * residual
        }
        residualRMS = (squaredResiduals / Double(points.count)).squareRoot()

        // The least-squares line passes through the weighted centroid, so anchor the map there.
        map = ClockMap(
            referenceLocal: last.local + Int64(meanX.rounded()),
            referenceOffset: last.offset + Int64(meanY.rounded()),
            slope: slope
        )
    }
}
