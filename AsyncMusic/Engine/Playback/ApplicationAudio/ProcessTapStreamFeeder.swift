#if os(macOS)
import AsyncMusicCore
import Foundation
import MacAudioCapture

/// Owns the capture graph on one worker queue; never calls Swift from the HAL callback.
nonisolated final class ProcessTapStreamFeeder: @unchecked Sendable {
    struct Stream: @unchecked Sendable {
        let buffer: StreamBuffer
        /// Host time of frame zero. Playback adds the same delay on every device.
        let epoch: Int64
    }

    var onReady: (@Sendable (Stream) -> Void)?
    var onFailure: (@Sendable (String) -> Void)?
    private let queue = DispatchQueue(label: "me.ktiays.asyncmusic.app-audio", qos: .userInitiated)
    private let application: AudioApplication
    private var capture: ApplicationAudioCapture?
    private var timer: DispatchSourceTimer?
    private var timeline: LiveAudioTimeline?
    private var buffer: StreamBuffer?
    private var announced = false
    private var nextProcessCheck: Int64 = 0
    private var left = [Float](repeating: 0, count: 4096)
    private var right = [Float](repeating: 0, count: 4096)

    init(application: AudioApplication) { self.application = application }

    func start() {
        queue.async { [self] in
            do {
                let capture = ApplicationAudioCapture(processes: application.processIDs, bundleIDs: application.bundleIDs)
                self.capture = capture
                try capture.start()
                let rate = capture.sampleRate
                buffer = StreamBuffer.make(sampleRate: rate, capacity: Int(rate * 12), startFrame: 0)
                timeline = LiveAudioTimeline(sampleRate: rate)
                nextProcessCheck = HostClock.now() + 1_000_000_000
                let timer = DispatchSource.makeTimerSource(queue: queue)
                timer.schedule(deadline: .now(), repeating: .milliseconds(5))
                timer.setEventHandler { [weak self] in self?.pump() }
                self.timer = timer
                timer.resume()
            } catch { fail(error.localizedDescription) }
        }
    }

    func stop() { queue.async { [self] in stopOnQueue() } }

    private func stopOnQueue() {
        timer?.cancel()
        timer = nil
        capture?.stop()
        capture = nil
        timeline = nil
        buffer = nil
    }

    private func fail(_ message: String) {
        stopOnQueue()
        onFailure?(message)
    }

    private func pump() {
        guard let capture, let buffer else { return }
        do {
            try capture.checkConfiguration()
            if #unavailable(macOS 26.0), HostClock.now() >= nextProcessCheck {
                nextProcessCheck = HostClock.now() + 1_000_000_000
                let current = Set(try AudioProcess.snapshot().map(\.objectID))
                if current.isDisjoint(with: application.processIDs) {
                    fail("The app's audio process stopped. Play audio in it, then start sharing again.")
                    return
                }
            }
            while let block = capture.read() {
                let hasSignal = block.samples.contains(where: { abs($0) > 0.0000001 })
                // A client may run silent IO long before the first sound. Keep waiting,
                // and anchor the group timeline to that first sound instead of a timeout.
                if !announced && !hasSignal { continue }
                // A clock discontinuity must start a new group timeline, not allocate an
                // unbounded silence segment or let devices play different source positions.
                if abs(block.hostTime - HostClock.now()) > 5_000_000_000 {
                    fail("App audio timing changed. Start app audio again.")
                    return
                }
                append(timeline!.push(block), to: buffer)
                if !announced, let epoch = timeline?.epoch {
                    announced = true
                    onReady?(Stream(buffer: buffer, epoch: epoch))
                }
            }
            append(timeline!.advanceSilence(to: HostClock.now()), to: buffer)
        } catch { fail(error.localizedDescription) }
    }

    private func append(_ segments: [LiveAudioSegment], to buffer: StreamBuffer) {
        for segment in segments {
            let count: Int
            switch segment {
            case let .samples(samples): count = samples.count / 2
            case let .silence(frames): count = frames
            }
            var offset = 0
            while offset < count {
                let n = min(4096, count - offset)
                for i in 0..<n {
                    if case let .samples(samples) = segment {
                        left[i] = samples[(offset+i)*2]
                        right[i] = samples[(offset+i)*2+1]
                    } else {
                        left[i] = 0; right[i] = 0
                    }
                }
                left.withUnsafeBufferPointer { l in
                    right.withUnsafeBufferPointer { r in
                        buffer.append(left: l.baseAddress!, right: r.baseAddress!, count: n)
                    }
                }
                offset += n
            }
        }
    }

    deinit { timer?.cancel(); capture?.stop() }
}
#endif
