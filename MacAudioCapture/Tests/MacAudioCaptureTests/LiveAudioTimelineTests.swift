import XCTest
@testable import MacAudioCapture

final class LiveAudioTimelineTests: XCTestCase {
    private func block(_ time: Int64, frames: Int = 480, value: Float = 0.25) -> CapturedAudioBlock {
        CapturedAudioBlock(hostTime: time, samples: [Float](repeating: value, count: frames*2))
    }

    func testEpochAndContiguousBlocks() {
        var timeline = LiveAudioTimeline(sampleRate: 48000)
        XCTAssertTrue(timeline.push(block(1_000_000_000)).isEmpty)
        let result = timeline.push(block(1_010_000_000))
        XCTAssertEqual(timeline.epoch, 1_000_000_000)
        XCTAssertEqual(timeline.endFrame, 480)
        guard case let .samples(samples) = result.first else { return XCTFail("Missing PCM") }
        XCTAssertEqual(samples.count, 960)
        XCTAssertTrue(samples.allSatisfy { $0 == 0.25 })
    }

    func testDeviceClockDriftMapsToHostTime() {
        var timeline = LiveAudioTimeline(sampleRate: 48000)
        for i in 0...1000 { _ = timeline.push(block(Int64(i) * 10_001_000)) }
        XCTAssertEqual(timeline.endFrame, 480048)
    }

    func testMissingCallbacksPreserveGapDuration() {
        var timeline = LiveAudioTimeline(sampleRate: 48000)
        _ = timeline.push(block(0))
        let result = timeline.push(block(1_000_000_000))
        XCTAssertEqual(timeline.endFrame, 48000)
        XCTAssertEqual(result.count, 2)
        guard case let .silence(count) = result.last else { return XCTFail("Missing gap") }
        XCTAssertEqual(count, 47520)
    }

    func testPauseAndResumeKeepOneTimeline() {
        var timeline = LiveAudioTimeline(sampleRate: 48000)
        _ = timeline.push(block(0))
        _ = timeline.advanceSilence(to: 1_100_000_000)
        XCTAssertEqual(timeline.endFrame, 48000)
        _ = timeline.push(block(2_000_000_000))
        _ = timeline.push(block(2_010_000_000))
        XCTAssertEqual(timeline.epoch, 0)
        XCTAssertEqual(timeline.endFrame, 96480)
    }

    func testStaleTimestampCannotRewindStream() {
        var timeline = LiveAudioTimeline(sampleRate: 48000)
        _ = timeline.push(block(0))
        _ = timeline.push(block(10_000_000))
        XCTAssertTrue(timeline.push(block(5_000_000)).isEmpty)
        XCTAssertEqual(timeline.endFrame, 480)
        _ = timeline.push(block(20_000_000))
        XCTAssertEqual(timeline.endFrame, 960)
    }
}
