#if os(macOS)
import Foundation
import NativeCapture
import Darwin

public struct AudioProcess: Sendable {
    public let objectID: UInt32
    public let pid: Int32
    public let bundleID: String
    public let executablePath: String
    public let isOutputRunning: Bool

    public static func snapshot() throws -> [AudioProcess] {
        var error: NSError?
        guard let values = AMAudioProcesses(&error) else {
            throw error ?? NSError(domain: "AsyncMusic.AppAudio", code: -1,
                                   userInfo: [NSLocalizedDescriptionKey: "Cannot list audio apps."])
        }
        return values.compactMap { value in
            guard let object = value["objectID"] as? NSNumber, let pid = value["pid"] as? NSNumber else { return nil }
            return AudioProcess(objectID: object.uint32Value, pid: pid.int32Value,
                                bundleID: value["bundleID"] as? String ?? "",
                                executablePath: value["path"] as? String ?? "",
                                isOutputRunning: (value["running"] as? NSNumber)?.boolValue ?? false)
        }
    }
}

public struct CapturedAudioBlock: Sendable {
    public let hostTime: Int64
    public let samples: [Float]
    public var frameCount: Int { samples.count / 2 }

    public init(hostTime: Int64, samples: [Float]) {
        self.hostTime = hostTime
        self.samples = samples
    }
}

/// Lifecycle and reads are confined to the caller's serial worker queue.
public final class ApplicationAudioCapture {
    private let native: AMProcessTap
    private var scratch = [Float](repeating: 0, count: 4096 * 2)
    private var timebase = mach_timebase_info_data_t()

    public init(processes: [UInt32], bundleIDs: [String]) {
        native = AMProcessTap(processes: processes.map { NSNumber(value: $0) }, bundleIDs: bundleIDs)
        mach_timebase_info(&timebase)
    }

    public var sampleRate: Double { native.sampleRate }
    public var droppedFrames: UInt64 { native.droppedFrames }

    public func start() throws { try native.start() }
    public func stop() { native.stop() }

    public func checkConfiguration() throws {
        let message: String
        switch native.problem {
        case 0: return
        case 1: message = "The app audio format changed. Start app audio again."
        case 2: message = "App audio has no valid timing information. Start app audio again."
        case 3: message = "App audio could not be processed in time. Start app audio again."
        default: message = "The audio device or service changed. Start app audio again."
        }
        throw NSError(domain: "AsyncMusic.AppAudio", code: native.problem,
                      userInfo: [NSLocalizedDescriptionKey: message])
    }

    public func read() -> CapturedAudioBlock? {
        var ticks: UInt64 = 0
        let frames = scratch.withUnsafeMutableBufferPointer {
            native.readFrames($0.baseAddress!, capacity: 4096, hostTime: &ticks)
        }
        guard frames > 0 else { return nil }
        let nanos = Int64(Double(ticks) * Double(timebase.numer) / Double(timebase.denom))
        return CapturedAudioBlock(hostTime: nanos, samples: Array(scratch.prefix(Int(frames) * 2)))
    }
}
#endif
